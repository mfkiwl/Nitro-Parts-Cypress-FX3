#include "cyu3error.h"
#include "cyu3usb.h"
#include "vendor_commands.h"
#include "rdwr.h"
#include "cpu_handler.h"
#include "slfifo_handler.h"
#include "main.h"
#include "fx3_terminals.h"
#include <cyu3i2c.h>
#include <m24xx.h>

#include "log.h"
#ifndef DEBUG_RDWR
#undef log_debug
#define log_debug(...) do {} while (0)
#endif

#ifdef FIRMWARE_DI
#include <di.h>
#endif

rdwr_cmd_t gRdwrCmd;
uint8_t gSerialNum[32] __attribute__ ((aligned (32))); // actually 16 bytes but DMACache requires multiple of 32
extern uint8_t glEp0Buffer[]; // dma aligned buffer for ep0 read/writes

void rdwr_teardown() {
  if(gRdwrCmd.handler) {
    switch(gRdwrCmd.handler->type) {
    case HANDLER_TYPE_CPU:
      cpu_handler_teardown();
      break;
    case HANDLER_TYPE_SLAVE_FIFO:
      slfifo_teardown();
      break;
    }
  }
}

#ifdef FIRMWARE_DI
#define mutex_log(...) do {} while(0)
//#define mutex_log log_info
/**
 * Fact: You can only unlock the mutex from the same thread that started it.
 * Fact: Slfifo transaction callbacks don't necessarily happen on the same thread.
 * Problem: slfifo transactions don't necessarily unlock the mutex
 * Solution: Call RDWR_DONE from the main thread.
 *           The firmware di thread calls this internally to it's get/set/read/write
 *           so doesn't have the same issue.
 **/
CyBool_t gRdwrLocked; // only true for nitro thread
void RDWR_DONE(CyBool_t main) {
  if (!main || gRdwrLocked) {
   mutex_log( "\n-%cU-\n", main ? 'm' : 'd' );
   if (main) gRdwrLocked=CyFalse;
   CyU3PMutexPut ( &gRdwrCmd.rdwr_mutex );
  }
}
#endif

/******************************************************************************/

CyU3PReturnStatus_t ep0_rdwr_setup() {
    // Fetch the rdwr command  
    // NOTE this api call acks the vendor command if it's successful
    CyU3PReturnStatus_t status = CyU3PUsbGetEP0Data(sizeof(rdwr_data_header_t), glEp0Buffer, 0);

    if(status != CY_U3P_SUCCESS){
      log_error("Error get EP0 Data\n", status);
      return status;
    }
    CyU3PMemCopy ( (uint8_t*)&gRdwrCmd.header, glEp0Buffer, sizeof(gRdwrCmd.header) );
    return CY_U3P_SUCCESS;
}

CyU3PReturnStatus_t handle_rdwr(uint8_t bReqType, uint16_t wValue, uint16_t wIndex, uint16_t wLength) {
  // NOTE wValue == term_addr
  // wIndex == 16 bits of transfer_length
  // wIndex is a hint for slave fifo if we need auto or manual mode
  // start a rdwr command based on incoming ep0 traffic
    //log_debug("Entering handleRDWR\n");
    if (bReqType != 0x40 || wLength != sizeof(rdwr_data_header_t)) {
      log_error("Bad ReqType or length=%d (%d)\n", wLength, sizeof(rdwr_data_header_t));
      return CY_U3P_ERROR_BAD_ARGUMENT;
    }

    RDWR_DONE(CyTrue); // if the last transaction failed go ahead and release the mutex before starting.
  
    return start_rdwr ( wValue, wIndex, ep0_rdwr_setup
    #ifdef FIRMWARE_DI
     , CyFalse
    #endif
    );
}


/*
 * This function initializes gRdwrCmd and initializes the correct handler.
 * It can be called directly by firmware internal features needing to start a 
 * transaction as long as the rdwr_setup function populates gRdwrCmd.header correctly.
 */

CyU3PReturnStatus_t start_rdwr( uint16_t term, uint16_t len_hint, rdwr_setup_handler rdwr_setup
#ifdef FIRMWARE_DI
 , CyBool_t firmware_di
#endif
) {
  CyU3PReturnStatus_t status=0;

 
 #ifdef FIRMWARE_DI
   if ((status=CyU3PMutexGet ( &gRdwrCmd.rdwr_mutex, 2000 )) != CY_U3P_SUCCESS) {
     log_warn ( "Mutex Lock Fail (%c)\n", firmware_di ? 'd' : 'm' );
     return status;
   }
   if (!firmware_di) gRdwrLocked = CyTrue; // track if we're locking from the main thread.
   mutex_log( "\n-%cL-\n", firmware_di ? 'd' : 'm' );
 #endif
 
  io_handler_t *new_handler = NULL;

  // Select the appropriate handler. Any handler specified with term_addr of
  // 0 is considered the wild card handler and will prevent any handlers
  // following if from being accessed.
  #ifdef FIRMWARE_DI
  if (firmware_di) {
   new_handler = &firmware_di_handler;
  } else {
  #endif
  int i = 0;
  while(io_handlers[i].type != HANDLER_TYPE_TERMINATOR) {
    if(io_handlers[i].term_addr == term ||
       io_handlers[i].term_addr == 0) { 
      new_handler = &(io_handlers[i]);
      log_debug("Found handler %d\n", i);
      break;
    }
    i++;
  }
  #ifdef FIRMWARE_DI
  }
  #endif
  
  // if we are switching handlers, uninit the previous handler
  // uninit function
  if(gRdwrCmd.handler != new_handler) {
    if (gRdwrCmd.handler && gRdwrCmd.handler->uninit_handler) {
        gRdwrCmd.handler->uninit_handler();
    }
  }

  // first tear down previous handlers DMA channels
  // only if we're switching handler types
  if(gRdwrCmd.handler && (
	!new_handler || 
 	gRdwrCmd.handler->type != new_handler->type )) {
    switch(gRdwrCmd.handler->type) {
    case HANDLER_TYPE_CPU:
      cpu_handler_teardown();
      break;
      
    case HANDLER_TYPE_SLAVE_FIFO:
      slfifo_teardown();
      break;
    #ifdef FIRMWARE_DI
    case HANDLER_TYPE_FDI:
      fdi_teardown();
      break;
    #endif
      
    default:
      // do nothing by default
      break;
    }
  }
  
  gRdwrCmd.handler = new_handler;

  // now setup the new handler types DMA channels
  if(gRdwrCmd.handler) {
    switch(gRdwrCmd.handler->type) {
    case HANDLER_TYPE_CPU:
      log_debug ( "setup the cpu handler please\n" );
      status=cpu_handler_setup();
      break;
      
    case HANDLER_TYPE_SLAVE_FIFO:
      status=slfifo_setup(len_hint % 4 == 0);
      break;
    #ifdef FIRMWARE_DI
    case HANDLER_TYPE_FDI:
      status=fdi_setup();
      break;
    #endif

    default:
      // do nothing by default
      log_warn( "Error no handler\n" );
      break;
    }
  }
  if (status) {
    log_error ( "gRdWrCmd.handler failed to setup. %d\n", status );
    return status; 
  }
  
  // NOTE see function documentation.  
  status = rdwr_setup();
  if (status) return status; 

  log_debug ( "rdwr command (%c) type: %d, term %d reg %d len %d (old done=%d tx=%d)\n",
#ifdef FIRMWARE_DI
      firmware_di ? 'd' : 'm', 
#else
      'm',
#endif
      gRdwrCmd.header.command,
      gRdwrCmd.header.term_addr,
      gRdwrCmd.header.reg_addr,
      gRdwrCmd.header.transfer_length,
      gRdwrCmd.done ? 1 : 0,
      gRdwrCmd.transfered_so_far );

  gRdwrCmd.done    = 0;
  gRdwrCmd.transfered_so_far = 0;
 
  // call the new handlers init function, if it exists
  if (gRdwrCmd.handler) {
    log_debug ( "handler type: %d\n", gRdwrCmd.handler->type );
    switch(gRdwrCmd.handler->type) {
    case HANDLER_TYPE_CPU:
      cpu_handler_cmd_start();
      break;
    #ifdef FIRMWARE_DI
    case HANDLER_TYPE_FDI: // uses slave fifo start
      log_debug ( "FDI start call slfifo start.\n" );
    #endif
    case HANDLER_TYPE_SLAVE_FIFO:
      slfifo_cmd_start();
      break;
    }
  } else {
    log_error ( "Handler is NULL\n" );
  }

  CyU3PEventSet(&glThreadEvent, NITRO_EVENT_DATA, CYU3P_EVENT_OR);

  return CY_U3P_SUCCESS;
}

/******************************************************************************/
CyBool_t handle_serial_num(uint8_t bReqType, uint16_t wLength) {
  CyU3PI2cPreamble_t preamble;
  uint32_t reg_addr = FX3_PROM_SERIALNUM0_0;
  uint8_t dev_addr = 0x50;
  uint8_t size = 17; // size of prom
  uint16_t status;

  if (wLength != 16) {
    log_error("Bad length=%d \n", wLength);
    return CY_U3P_ERROR_BAD_ARGUMENT;
  }

  switch(bReqType) {
  case 0xC0:
    preamble.length    = 4;
    preamble.buffer[0] = m24xx_get_dev_addr(dev_addr, reg_addr, size, 0);
    preamble.buffer[1] = (uint8_t)(reg_addr >> 8);
    preamble.buffer[2] = (uint8_t)(reg_addr & 0xFF);
    preamble.buffer[3] = m24xx_get_dev_addr(dev_addr, reg_addr, size, 1);
    preamble.ctrlMask  = 0x0004;
    status = CyU3PI2cReceiveBytes (&preamble, gSerialNum, 16, 1);
    if(status) {
      log_error("Error reading serial num from prom (%d)\n", status);
      return CyFalse;
    }
    status = CyU3PUsbSendEP0Data(16, gSerialNum);
    if(status) {
      log_error("Error Sending serial num to EP0 (%d)\n", status);
      return CyFalse;
    }
    return CyTrue;

  case 0x40:
    status = CyU3PUsbGetEP0Data(wLength, gSerialNum, 0);
    if(status) {
      log_error("Error getting serial num from EP0 (%d)\n", status);
      return status;
    }

    preamble.length    = 3;
    preamble.buffer[0] = m24xx_get_dev_addr(dev_addr, reg_addr, size, 0);
    preamble.buffer[1] = (uint8_t)(reg_addr >> 8);
    preamble.buffer[2] = (uint8_t)(reg_addr & 0xFF);
    preamble.ctrlMask  = 0x0000;
    
    status = CyU3PI2cTransmitBytes(&preamble, gSerialNum, 16, 1);
    if(status) {
      log_error("Error writing serial num to I2C (%d)\n", status);
      return CyFalse;
    }
    
    /* Wait for the write to complete. */
    preamble.length = 1;
    status = CyU3PI2cWaitForAck(&preamble, 200);
    if(status) {
      log_error("Error waiting for i2c ACK after writing serial num (%d)\n", status);
      return CyFalse;
    }
    
    /* An additional delay seems to be required after receiving an ACK. */
    CyU3PThreadSleep (1);
    return CyTrue;


  default:
    log_error("Bad ReqType=%d \n", bReqType);
    return CY_U3P_ERROR_BAD_ARGUMENT;
  }
}


/******************************************************************************/
CyBool_t handle_vendor_cmd(uint8_t  bRequest, uint8_t bReqType,
			   uint8_t  bType, uint8_t bTarget,
			   uint16_t wValue, uint16_t wIndex, 
			   uint16_t wLength) {

  //log_debug("Entering handle_vendor_cmd\n");
  CyBool_t isHandled = CyTrue;
  CyU3PReturnStatus_t status = CY_U3P_SUCCESS;

  //  log_debug("VC%x\n", bRequest);
  switch (bRequest) {
  case VC_HI_RDWR:
    log_debug("Call handle_rdwr\n");
    status = handle_rdwr(bReqType, wValue, wIndex, wLength);
    break;

  case VC_SERIAL:
    status = handle_serial_num(bReqType, wLength);
    break;

//  case VC_RDWR_RAM:
//    status = handleRamRdwr(bRequest, bReqType, bType, bTarget, wValue, wIndex, wLength);
//    break;

  case VC_RENUM:
    CyU3PDeviceReset(CyFalse); // cold boot from prom
    break; // for readability but the above function actually doesn't return.
    
  default:
    isHandled = CyFalse;
    break;
  }

  if ((isHandled != CyTrue) || (status != CY_U3P_SUCCESS)) {
    /* This is an unhandled setup command. Stall the EP. */
    log_debug("VC stalled\n" ); // (cmd: %d)\n", bRequest);
    CyU3PUsbStall (0, CyTrue, CyFalse);
  }

  log_debug ( "handle_vendor_cmd exit\n");
  return CyTrue;
}
