/**
 * \file
 *
 * \brief GMAC (Ethernet MAC) driver for SAM.
 *
 * Copyright (c) 2015-2016 Atmel Corporation. All rights reserved.
 *
 * \asf_license_start
 *
 * \page License
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * 3. The name of Atmel may not be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * 4. This software may only be redistributed and used in connection with an
 *    Atmel microcontroller product.
 *
 * THIS SOFTWARE IS PROVIDED BY ATMEL "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT ARE
 * EXPRESSLY AND SPECIFICALLY DISCLAIMED. IN NO EVENT SHALL ATMEL BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * \asf_license_stop
 *
 */

/*
 * Support and FAQ: visit <a href="http://www.atmel.com/design-support/">Atmel Support</a>
 */


/* Standard includes. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* FreeRTOS includes. */
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

#include "FreeRTOSIPConfig.h"

/* FreeRTOS+TCP includes. */
#include "FreeRTOS_IP.h"
#include "FreeRTOS_Sockets.h"
#include "FreeRTOS_IP_Private.h"
#include "FreeRTOS_ARP.h"
#include "NetworkBufferManagement.h"
#include "NetworkInterface.h"

#include "compiler.h"
#include "gmac.h"
#include "pmc.h"

#include "conf_eth.h"
#include "gmac_handler.h"

/* This file is included to see if 'CONF_BOARD_ENABLE_CACHE' is defined. */
//#include "conf_board.h"
#include "core_cm7.h"

#ifndef ARRAY_SIZE
    #define ARRAY_SIZE( x )    ( int ) ( sizeof( x ) / sizeof( x )[ 0 ] )
#endif

#if ( GMAC_RX_BUFFERS <= 1 )
    #error Configuration error
#endif

#if ( GMAC_TX_BUFFERS <= 1 )
    #error Configuration error
#endif

/**
 * \defgroup gmac_group Ethernet Media Access Controller
 *
 * See \ref gmac_quickstart.
 *
 * Driver for the GMAC (Ethernet Media Access Controller).
 * This file contains basic functions for the GMAC, with support for all modes, settings
 * and clock speeds.
 *
 * \section dependencies Dependencies
 * This driver does not depend on other modules.
 *
 * @{
 */

/** TX descriptor lists */
//__attribute__( ( section( ".first_data" ) ) )
COMPILER_ALIGNED( 8 )
static gmac_tx_descriptor_t gs_tx_desc[ GMAC_TX_BUFFERS ] = {};

//__attribute__( ( section( ".first_data" ) ) )
COMPILER_ALIGNED( 8 )
static gmac_tx_descriptor_t gs_tx_desc_null = {};

/** RX descriptors lists */
//__attribute__( ( section( ".first_data" ) ) )
COMPILER_ALIGNED( 8 )
static gmac_rx_descriptor_t gs_rx_desc[ GMAC_RX_BUFFERS ];

/** Return count in buffer */
#define CIRC_CNT( head, tail, size )      ( ( ( head ) - ( tail ) ) % ( size ) )

/*
 * Return space available, from 0 to size-1.
 * Always leave one free char as a completely full buffer that has (head == tail),
 * which is the same as empty.
 */
#define CIRC_SPACE(head, tail, size)    CIRC_CNT((tail), ((head) + 1), (size))

/** Circular buffer is empty ? */
#define CIRC_EMPTY(head, tail )          ( ( head ) == ( tail ) )
/** Clear circular buffer */
#define CIRC_CLEAR(head, tail )          do { ( head ) = 0; ( tail ) = 0; } while( 0 )

/* Two call-back functions that should be defined in NetworkInterface.c */
extern void xRxCallback(uint32_t ulStatus, BaseType_t* task_switch_required);
extern void xTxCallback(uint8_t * puc_buffer, BaseType_t* task_switch_required);
extern void returnTxBuffer(uint8_t * puc_buffer);


/** Increment head or tail */
static __inline void circ_inc32(int32_t * lHeadOrTail, uint32_t ulSize)
{
    ( *lHeadOrTail )++;

    if( ( *lHeadOrTail ) >= ( int32_t ) ulSize )
    {
        ( *lHeadOrTail ) = 0;
    }
}

/**
 * \brief Disable transfer, reset registers and descriptor lists.
 *
 * \param p_dev Pointer to GMAC driver instance.
 *
 */
void gmac_reset_tx_mem(gmac_device_t* p_dev)
{
    /* Disable TX */
    gmac_enable_transmit(p_dev->p_hw, 0U);

    for(uint32_t ul_index = 0; ul_index < ARRAY_SIZE( gs_tx_desc ); ul_index++ )
    {
        uint32_t ulAddr = gs_tx_desc[ul_index].addr;

        if (ulAddr)
        {
            returnTxBuffer((uint8_t *)ulAddr );
        }
    }

    /* Set up the TX descriptors */
    CIRC_CLEAR( p_dev->l_tx_head, p_dev->l_tx_tail );

    for(uint32_t ul_index = 0; ul_index < GMAC_TX_BUFFERS; ul_index++ )
    {
        gs_tx_desc[ ul_index ].addr = 0U;
        gs_tx_desc[ ul_index ].status.val = GMAC_TXD_USED;
    }

    /* Set the WRAP bit in the last descriptor. */
    gs_tx_desc[GMAC_TX_BUFFERS - 1].status.val = GMAC_TXD_USED | GMAC_TXD_WRAP;

    /* Set transmSAME70Sit buffer queue */
    gmac_set_tx_queue(p_dev->p_hw, ( uint32_t ) gs_tx_desc );

    gmac_set_tx_priority_queue(p_dev->p_hw, ( uint32_t )&gs_tx_desc_null, GMAC_QUE_1);
    gmac_set_tx_priority_queue(p_dev->p_hw, ( uint32_t )&gs_tx_desc_null, GMAC_QUE_2);
    // Note that SAME70 REV B had 6 priority queues.
    gmac_set_tx_priority_queue(p_dev->p_hw, ( uint32_t )&gs_tx_desc_null, GMAC_QUE_3);
    gmac_set_tx_priority_queue(p_dev->p_hw, ( uint32_t )&gs_tx_desc_null, GMAC_QUE_4);
    gmac_set_tx_priority_queue(p_dev->p_hw, ( uint32_t )&gs_tx_desc_null, GMAC_QUE_5);

}

/**
 * \brief Disable receiver, reset registers and descriptor list.
 *
 * \param p_dev Pointer to GMAC Driver instance.
 */
static void gmac_reset_rx_mem(gmac_device_t* p_dev)
{
    /* Disable RX */
    gmac_enable_receive(p_dev->p_hw, 0);

    /* Set up the RX descriptors */
    p_dev->ul_rx_idx = 0;

    for (uint32_t ul_index = 0; ul_index < GMAC_RX_BUFFERS; ul_index++)
    {
        NetworkBufferDescriptor_t * pxNextNetworkBufferDescriptor;

        pxNextNetworkBufferDescriptor = pxGetNetworkBufferWithDescriptor(GMAC_RX_UNITSIZE, 0UL);
        configASSERT(pxNextNetworkBufferDescriptor != nullptr);
        uint32_t ul_address = ( uint32_t ) (pxNextNetworkBufferDescriptor->pucEthernetBuffer);

        gs_rx_desc[ul_index].addr.val = ul_address & GMAC_RXD_ADDR_MASK;
        gs_rx_desc[ul_index].status.val = 0;
    }

    /* Set the WRAP bit in the last descriptor. */
    gs_rx_desc[GMAC_RX_BUFFERS - 1].addr.bm.b_wrap = 1;

    /* Set receive buffer queue */
    gmac_set_rx_queue(p_dev->p_hw, (uint32_t)gs_rx_desc);
}


/**
 * \brief Initialize the allocated buffer lists for GMAC driver to transfer data.
 * Must be invoked after gmac_dev_init() but before RX/TX starts.
 *
 * \note If input address is not 8-byte aligned, the address is automatically
 *       adjusted and the list size is reduced by one.
 *
 * \param p_gmac Pointer to GMAC instance.
 * \param p_gmac_dev Pointer to GMAC device instance.
 * \param p_dev_mm Pointer to the GMAC memory management control block.
 *
 * \return GMAC_OK or GMAC_PARAM.
 */
static uint8_t gmac_init_mem(Gmac * p_gmac, gmac_device_t * p_gmac_dev )
{
    /* Reset TX & RX Memory */
    gmac_reset_rx_mem(p_gmac_dev);
    gmac_reset_tx_mem(p_gmac_dev);

    /* Enable Rx and Tx, plus the statistics register */
    gmac_enable_transmit(p_gmac, true);
    gmac_enable_receive(p_gmac, true);
    gmac_enable_statistics_write(p_gmac, true);

    /* Set up the interrupts for transmission and errors */
    gmac_enable_interrupt(p_gmac,
                          GMAC_IER_RLEX |   /* Enable retry limit  exceeded interrupt. */
                          GMAC_IER_RCOMP |  /* Enable receive complete interrupt. */
                          GMAC_IER_RXUBR |  /* Enable receive used bit read interrupt. */
                          GMAC_IER_ROVR |   /* Enable receive overrun interrupt. */
                          GMAC_IER_TCOMP |  /* Enable transmit complete interrupt. */
                          GMAC_IER_TUR |    /* Enable transmit underrun interrupt. */
                          GMAC_IER_TFC |    /* Enable transmit buffers exhausted in mid-frame interrupt. */
                          GMAC_IER_HRESP |  /* Enable Hresp not OK interrupt. */
                          GMAC_IER_PFNZ |   /* Enable pause frame received interrupt. */
                          GMAC_IER_PTZ);    /* Enable pause time zero interrupt. */

    return GMAC_OK;
}

/**
 * \brief Initialize the GMAC driver.
 *
 * \param p_gmac   Pointer to the GMAC instance.
 * \param p_gmac_dev Pointer to the GMAC device instance.
 * \param p_opt GMAC configure options.
 */
void gmac_dev_init(Gmac* p_gmac, gmac_device_t* p_gmac_dev)
{
    /* Disable TX & RX and more */
    gmac_network_control(p_gmac, 0);
    gmac_disable_interrupt(p_gmac, ~0U);

    gmac_clear_statistics(p_gmac);

    /* Clear all status bits in the receive status register. */
    gmac_clear_rx_status(p_gmac, GMAC_RSR_RXOVR | GMAC_RSR_REC | GMAC_RSR_BNA | GMAC_RSR_HNO);

    /* Clear all status bits in the transmit status register */
    gmac_clear_tx_status(p_gmac,
        GMAC_TSR_UBR | GMAC_TSR_COL | GMAC_TSR_RLE | GMAC_TSR_TXGO | GMAC_TSR_TFC | GMAC_TSR_TXCOMP | GMAC_TSR_HRESP);


    /* Clear interrupts */
    gmac_get_interrupt_status(p_gmac);

    /* Enable the copy of data into the buffers
     * ignore broadcasts, and not copy FCS. */

    gmac_set_config(p_gmac,
                    //(gmac_get_config( p_gmac ) & ~GMAC_NCFGR_RXBUFO_Msk) |
                    GMAC_NCFGR_RFCS |                                //  Remove FCS, frame check sequence (last 4 bytes).
                    GMAC_NCFGR_RXBUFO(ETHERNET_CONF_DATA_OFFSET)  |  // Set Ethernet Offset.
                    GMAC_NCFGR_RXCOEN |                              // RXCOEN related function.
                    GMAC_NCFGR_MAXFS |                               // 1536 Maximum Frame Size.
                    GMAC_NCFGR_FD    |                               // Full-Duplex.
                    GMAC_NCFGR_DCPF);                                // Disable Copy of Pause Frames.


    /*
     * GMAC_DCFGR_TXCOEN: (GMAC_DCFGR) Transmitter Checksum Generation Offload Enable.
     * Note: SAM4E/SAME70 do have RX checksum offloading
     * but TX checksum offloading has NOT been implemented,
     * at least on a SAM4E.
     * http://community.atmel.com/forum/sam4e-gmac-transmit-checksum-offload-enablesolved
     */
    uint32_t ulValue = gmac_get_dma(p_gmac);

    /* Let the GMAC set TX checksum's. */
    ulValue |= GMAC_DCFGR_TXCOEN;
    /* Transmitter Packet Buffer Memory Size Select:
     * Use full configured addressable space (4 Kbytes). */
    ulValue |= GMAC_DCFGR_TXPBMS;

    /* These bits determines the size of buffer to use in main AHB system memory when writing received data. The value
    is defined in multiples of 64 bytes. Clear the DMA Receive Buffer Size (DRBS) field: */
    ulValue &= ~(GMAC_DCFGR_DRBS_Msk);
    /* And set it: */
    ulValue |= GMAC_DCFGR_DRBS(GMAC_RX_UNITSIZE / 64U);

    gmac_set_dma(p_gmac, ulValue);

    /* Enable/Disable Copy(Receive) All Valid Frames. */
    gmac_enable_copy_all(p_gmac, false);

    /* Disable/Enable broadcast receiving */
    gmac_disable_broadcast(p_gmac, false);

    /* Initialize memory */
    gmac_init_mem(p_gmac, p_gmac_dev);

    /* Set Mac Address */
    gmac_set_address(p_gmac, 0, const_cast<uint8_t*>(FreeRTOS_GetMACAddress()));
}

/**
 * \brief Frames can be read from the GMAC in multiple sections.
 *
 * Returns > 0 if a complete frame is available
 * It also it cleans up incomplete older frames
 */

static uint32_t gmac_dev_poll(gmac_device_t* p_gmac_dev)
{
    uint32_t ulReturn = 0;
    int32_t ulIndex = p_gmac_dev->ul_rx_idx;
    gmac_rx_descriptor_t * pxHead = &gs_rx_desc[ ulIndex ];

    while ((pxHead->addr.val & GMAC_RXD_OWNERSHIP) != 0)
    {
        if ((pxHead->status.val & (GMAC_RXD_SOF | GMAC_RXD_EOF)) == (GMAC_RXD_SOF | GMAC_RXD_EOF))
        {
            /* Here a complete frame in a single segment. */
            ulReturn = pxHead->status.bm.b_len;
            break;
        }

        /* Return the buffer to DMA. */
        pxHead->addr.bm.b_ownership = 0;

        /* Let ulIndex/pxHead point to the next buffer. */
        circ_inc32( &ulIndex, GMAC_RX_BUFFERS );
        pxHead = &gs_rx_desc[ ulIndex ];
        /* And remember this index. */
        p_gmac_dev->ul_rx_idx = ulIndex;
    }

    return ulReturn;
}

/**
 * \brief Frames can be read from the GMAC in multiple sections.
 * Read ul_frame_size bytes from the GMAC receive buffers to pcTo.
 * p_rcv_size is the size of the entire frame.  Generally gmac_read
 * will be repeatedly called until the sum of all the ul_frame_size equals
 * the value of p_rcv_size.
 *
 * \param p_gmac_dev Pointer to the GMAC device instance.
 * \param p_frame Address of the frame buffer.
 * \param ul_frame_size  Length of the frame.
 * \param p_rcv_size   Received frame size.
 *
 * \return GMAC_OK if receiving frame successfully, otherwise failed.
 */
uint32_t gmac_dev_read(gmac_device_t * p_gmac_dev, uint8_t * p_frame, uint32_t ul_frame_size, uint32_t* p_rcv_size,
    uint8_t** pp_recv_frame)
{
    int32_t nextIdx; /* A copy of the Rx-index 'ul_rx_idx' */
    int32_t bytesLeft = gmac_dev_poll( p_gmac_dev );
    gmac_rx_descriptor_t * pxHead;

    if( bytesLeft == 0 )
    {
        return GMAC_RX_NO_DATA;
    }

    /* gmac_dev_poll has confirmed that there is a complete frame at
     * the current position 'ul_rx_idx'
     */
    nextIdx = p_gmac_dev->ul_rx_idx;

    /* Read +2 bytes because buffers are aligned at -2 bytes */
    #define min(a,b) (((a)<(b))?(a):(b))
    bytesLeft = min(bytesLeft + 2, (int32_t)ul_frame_size );
    #undef min

    #if ( __DCACHE_PRESENT != 0 ) && defined( CONF_BOARD_ENABLE_CACHE )
        SCB_InvalidateDCache();
    #endif

    if( p_frame != NULL )
    {
        /* Return a pointer to the earlier DMA buffer. */
        *( pp_recv_frame ) = ( uint8_t * )
                             ( ( ( gs_rx_desc[ nextIdx ].addr.val ) & ~( 0x03ul ) ) + 2 );
        /* Set the new DMA-buffer. */
        gs_rx_desc[ nextIdx ].addr.bm.addr_dw = ( ( uint32_t ) p_frame ) / 4;
    }
    else
    {
        /* The driver could not allocate a buffer to receive a packet.
         * Leave the current DMA buffer in place. */
        configASSERT(false);
    }

    do
    {
        pxHead = &gs_rx_desc[ nextIdx ];
        pxHead->addr.val &= ~( GMAC_RXD_OWNERSHIP );
        circ_inc32( &nextIdx, GMAC_RX_BUFFERS );
    } while ((pxHead->status.val & GMAC_RXD_EOF) == 0);

    p_gmac_dev->ul_rx_idx = nextIdx;

    *p_rcv_size = bytesLeft;

    return GMAC_OK;
}

/**
 * \brief Send ulLength bytes from pcFrom. This copies the buffer to one of the
 * GMAC Tx buffers, and then indicates to the GMAC that the buffer is ready.
 * If lEndOfFrame is true then the data being copied is the end of the frame
 * and the frame can be transmitted.
 *
 * \param p_gmac_dev Pointer to the GMAC device instance.
 * \param p_buffer       Pointer to the data buffer.
 * \param ul_size    Length of the frame.
 *
 * \return Length sent.
 */
uint32_t gmac_dev_write(gmac_device_t * p_gmac_dev, void * p_buffer, uint32_t ul_size)
{
    /* Check parameter */
    if (ul_size > GMAC_TX_UNITSIZE)
    {
        return GMAC_PARAM;
    }

    /* Pointers to the current transmit descriptor */
    volatile gmac_tx_descriptor_t* p_tx_td = &gs_tx_desc[p_gmac_dev->l_tx_head];

    /* If no free TxTd, buffer can't be sent, schedule the wakeup callback */
    if( ( p_tx_td->status.val & GMAC_TXD_USED ) == 0 )
    {
        return GMAC_TX_BUSY;
    }

    /* Set up/copy data to transmission buffer */
    if (p_buffer && ul_size)
    {
        /* Driver manages the ring buffer */

        /* Calculating the checksum here is faster than calculating it from the GMAC buffer
         * because within p_buffer, it is well aligned */
        p_tx_td->addr = (uint32_t)p_buffer;
    }

    /* Update transmit descriptor status */

    /* The buffer size defined is the length of ethernet frame,
     * so it's always the last buffer of the frame. */
    if (p_gmac_dev->l_tx_head == (int32_t)(GMAC_TX_BUFFERS - 1 ))
    {
        /* No need to 'and' with GMAC_TXD_LEN_MASK because ul_size has been checked
         * GMAC_TXD_USED will now be cleared. */
        p_tx_td->status.val = (ul_size & GMAC_TXD_LEN_MASK) | GMAC_TXD_LAST | GMAC_TXD_WRAP;
    }
    else
    {
        /* GMAC_TXD_USED will now be cleared. */
        p_tx_td->status.val = (ul_size & GMAC_TXD_LEN_MASK) | GMAC_TXD_LAST;
    }

    circ_inc32( &p_gmac_dev->l_tx_head, GMAC_TX_BUFFERS );

    /* Now start to transmit if it is still not done */
    gmac_start_transmission(p_gmac_dev->p_hw);

    return GMAC_OK;
}

/**
 * \brief Get current load of transmit.
 *
 * \param p_gmac_dev Pointer to the GMAC device instance.
 *
 * \return Current load of transmit.
 */
uint32_t gmac_dev_get_tx_load(gmac_device_t * p_gmac_dev)
{
    uint16_t us_head = p_gmac_dev->l_tx_head;
    uint16_t us_tail = p_gmac_dev->l_tx_tail;

    return CIRC_CNT(us_head, us_tail, GMAC_TX_BUFFERS);
}

/**
 * \brief Reset TX & RX queue & statistics.
 *
 * \param p_gmac_dev   Pointer to GMAC device instance.
 */
void gmac_dev_reset( gmac_device_t * p_gmac_dev )
{
    Gmac * p_hw = p_gmac_dev->p_hw;

    gmac_reset_rx_mem( p_gmac_dev );
    gmac_reset_tx_mem( p_gmac_dev );
    gmac_network_control(p_hw, GMAC_NCR_TXEN | GMAC_NCR_RXEN | GMAC_NCR_WESTAT | GMAC_NCR_CLRSTAT);
}

void gmac_dev_halt( Gmac * p_gmac )
{
    gmac_network_control(p_gmac, GMAC_NCR_WESTAT | GMAC_NCR_CLRSTAT);
    gmac_disable_interrupt(p_gmac, ~0u);
}

/**
 * \brief GMAC Interrupt handler.
 *
 * \param p_gmac_dev   Pointer to GMAC device instance.
 */
void gmac_handler(gmac_device_t* p_gmac_dev, BaseType_t* task_switch_required)
{
    Gmac * p_hw = p_gmac_dev->p_hw;

    gmac_tx_descriptor_t * p_tx_td;

    uint32_t ul_isr = gmac_get_interrupt_status(p_hw);
    //ul_isr &= ~(gmac_get_interrupt_mask(p_hw) | 0xF8030300);

    uint32_t ul_rsr = gmac_get_rx_status(p_hw);
    uint32_t ul_tsr = gmac_get_tx_status(p_hw);

    /* RX packet */
    if ((ul_isr & GMAC_ISR_RCOMP) || (ul_rsr & (GMAC_RSR_REC | GMAC_RSR_RXOVR | GMAC_RSR_BNA)))
    {
        /* Clear status */
        gmac_clear_rx_status(p_hw, ul_rsr);

        if (ul_isr & GMAC_ISR_RCOMP)
        {
            ul_rsr |= GMAC_RSR_REC;
        }

        /* Invoke callbacks which can be useful to wake up a task */
        xRxCallback(ul_rsr, task_switch_required);
    }

    /* TX packet */
    if ((ul_isr & GMAC_ISR_TCOMP) || (ul_tsr & (GMAC_TSR_TXCOMP | GMAC_TSR_COL | GMAC_TSR_RLE)))
    {
        /* A frame transmitted */

        /* Check RLE */
        if( ul_tsr & GMAC_TSR_RLE )
        {
            /* Status RLE & Number of discarded buffers */
            gmac_reset_tx_mem(p_gmac_dev);
            gmac_enable_transmit(p_hw, 1);
        }

        /* Clear status */
        gmac_clear_tx_status(p_hw, ul_tsr);

        if (!CIRC_EMPTY(p_gmac_dev->l_tx_head, p_gmac_dev->l_tx_tail))
        {
            /* Check the buffers */
            do
            {
                p_tx_td = &gs_tx_desc[ p_gmac_dev->l_tx_tail ];

                /* Any error? Exit if buffer has not been sent yet */
                if( ( p_tx_td->status.val & GMAC_TXD_USED ) == 0 )
                {
                    break;
                }

                /* Notify upper layer that a packet has been sent */
                xTxCallback((uint8_t *)p_tx_td->addr, task_switch_required); /* Function call prvTxCallback */

                p_tx_td->addr = 0ul;

                circ_inc32( &p_gmac_dev->l_tx_tail, GMAC_TX_BUFFERS );
            } while (CIRC_CNT(p_gmac_dev->l_tx_head, p_gmac_dev->l_tx_tail, GMAC_TX_BUFFERS));
        }

        /*
        if (ul_tsr & GMAC_TSR_RLE)
        {
            // Notify upper layer RLE.
            xTxCallback(nullptr, task_switch_required);
        }*/
    }
}
