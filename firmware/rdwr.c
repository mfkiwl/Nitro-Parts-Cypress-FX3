#include <cyu3error.h>
#include <cyu3usb.h>
#ifdef CX3
#include <cyu3mipicsi.h>
#endif
#include "vendor_commands.h"
#include "rdwr.h"
#include "main.h"
//#include "fx3_terminals.h"
//#include <cyu3i2c.h>
//#include <m24xx.h>

// provided by serial.c
// or another file
extern uint16_t set_serial(uint8_t*);
extern uint16_t get_serial(uint8_t*);

#include "log.h"
#ifndef DEBUG_RDWR
#undef log_debug
#define log_debug(...) do {} while (0)
#endif

#ifdef FIRMWARE_DI
#include <di.h>
#endif

rdwr_cmd_t gRdwrCmd;
uint16_t gRdwrCmdInitStat=0;
//uint8_t gSerialNum[32] __attribute__ ((aligned (32))); // actually 16 bytes but DMACache requires multiple of 32
extern uint8_t glEp0Buffer[]; // dma aligned buffer for ep0 read/writes

void rdwr_teardown() {
  gRdwrCmd.done=1;
  if(gRdwrCmd.io_handler && gRdwrCmd.io_handler->uninit_handler) {
    gRdwrCmd.io_handler->uninit_handler();
  }
  if(gRdwrCmd.io_handler && gRdwrCmd.io_handler->handler->handler_teardown) {
      gRdwrCmd.io_handler->handler->handler_teardown();
  }
  gRdwrCmd.io_handler=NULL;
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

 // TODO not sure w/ new handlers how broken the firmware di handler is
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
  // the passed in term or the handler_filter returns True
  #ifdef FIRMWARE_DI
  if (firmware_di) {
   new_handler = &firmware_di_handler;
  } else {
  #endif
  int i = 0;
  while(io_handlers[i].handler) {
    if ((io_handlers[i].handler->handler_filter &&
         io_handlers[i].handler->handler_filter(term)) ||
        io_handlers[i].term_addr == term) {
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
  if(gRdwrCmd.io_handler != new_handler) {
    log_debug ( "uninit previous handler\n");
    if (gRdwrCmd.io_handler && gRdwrCmd.io_handler->uninit_handler) {
        gRdwrCmd.io_handler->uninit_handler();
    }
  }

  // first tear down previous handlers DMA channels
  // only if we're switching handler types
  if(gRdwrCmd.io_handler && (
	!new_handler ||
 	gRdwrCmd.io_handler->handler != new_handler->handler )) {
    log_debug ( "switching handler types, teardown old handler\n");
    if (gRdwrCmd.io_handler->handler->handler_teardown)
        gRdwrCmd.io_handler->handler->handler_teardown();
  }

  // now setup the new handler types DMA channels
  // if switching types
  // handlers internally must track if they're already set up
  // this function calls the new setup regardless
  if (new_handler &&
      new_handler->handler->handler_setup) {
        log_debug ( "setup new handler type\n");
        status=new_handler->handler->handler_setup(len_hint);
        if (status) {
          log_error ( "gRdWrCmd.io_handler failed to setup. %d\n", status );
          return status;
        }
  }

  gRdwrCmd.io_handler = new_handler;

  // dma channels should be set up at this point,
  // ack the vender
  // some handlers init need to know header info
  // so ack vendor command now

  // NOTE see function documentation.
  // TODO - do we need a mutex to stop the
  // data thread from calling read or write before the
  // init below is done?
  status = rdwr_setup();
  if (status) return status;

  // rest of the header besides done
  gRdwrCmd.transfered_so_far = 0;


  // NOTE from here on..
  // return value should be 0 regardless of status
  // on init funcs because the vendor command has been acked.
  // returning a status code will not report to the driver
  // an error has occurred

  // init called every time on new trans
  // should it be?  not symetric api with uninit
  // TODO add start/finish and change init/uninit to only
  // be when handler changes?
  if (gRdwrCmd.io_handler && gRdwrCmd.io_handler->init_handler)
    {
    log_debug ( "init new handler\n");
    status=gRdwrCmd.io_handler->init_handler();
    if (status) {
      gRdwrCmdInitStat=status;
      log_error ( "handler fail to init %d\n", status);
      return 0;
   }
  }

  // call the new handlers start function, if it exists
  if (gRdwrCmd.io_handler) {
     if (gRdwrCmd.io_handler->handler->handler_start) {
        status = gRdwrCmd.io_handler->handler->handler_start();
        if (status) {
          gRdwrCmdInitStat=status;
          log_error ( "handler_start fail %d\n", status);
          return 0;
        }
     }
  } else {
    log_error ( "Handler is NULL\n" );
  }

  // TODO, the data thread will start runnign the dma callbacks
  // once done==0.  Enough to put this at the end or do we need
  // a mutex.
  // TODO: slfifo handler ignores done and starts as soon as it's init_handler
  // is run.
  gRdwrCmd.done    = 0;


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

  CyU3PEventSet(&glThreadEvent, NITRO_EVENT_DATA, CYU3P_EVENT_OR);

  return 0;
}

/******************************************************************************/
CyBool_t gSerialCached=CyFalse;
uint8_t gSerialNum[16];

#ifdef ENABLE_LOGGING
#define log_serial() \
  do { \
  int i; \
  for (i=0;i<16;i+=2) \
    log_debug ( "%c", glEp0Buffer[i] ); \
  } while (0);
#else
#define log_serial()
#endif

uint16_t cache_serial() {
    uint16_t ret;

    log_debug ( "Return prom serial.." );
    ret = get_serial(glEp0Buffer);
    if (ret) return ret;
    log_serial();
    CyU3PMemCopy(gSerialNum,glEp0Buffer,16);
    gSerialCached=CyTrue;
    return 0;
}
//
// internal method for retrieving serial number which may be cached
uint16_t rdwr_get_serial(uint8_t *buf) {
    CyU3PReturnStatus_t status;
    if (!gSerialCached) {
        status=cache_serial();
        if (!status) return status;
    }
    CyU3PMemCopy(buf,gSerialNum,16);
    log_debug ( "Return cached serial.." );
    log_serial();
    return 0;
}

CyU3PReturnStatus_t handle_serial_num(uint8_t bReqType, uint16_t wLength) {


  CyU3PReturnStatus_t status;

  if (wLength != 16) {
      log_error ( "Bad serial number length: %d\n", wLength);
      return CY_U3P_ERROR_BAD_ARGUMENT;
  }

  switch (bReqType) {
      case 0xc0: // get serial
          status = rdwr_get_serial(glEp0Buffer);
          if (status) return status;

            status = CyU3PUsbSendEP0Data(16, glEp0Buffer);
            if (status) {
              log_error("Error Sending serial num to EP0 (%d)\n", status);
              return CyFalse;
            }
            break;
      case 0x40: // set_serial
            status = CyU3PUsbGetEP0Data(16, glEp0Buffer, 0);
            if (status) {
                log_error("Error getting serial num from EP0 (%d)\n", status);
                return status;
            }

            log_info ( "Setting new serial number: " );
            log_serial();

            status = set_serial(glEp0Buffer);
            if (status) return status;

            log_info ( "... ok\n" );

            CyU3PMemCopy(gSerialNum,glEp0Buffer,16);
            gSerialCached=CyTrue;
            break;
        default:
            return CY_U3P_ERROR_BAD_OPTION;
  }

  return 0;
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

  case VC_RENUM:

    CyU3PEventSet(&glThreadEvent, NITRO_EVENT_REBOOT, CYU3P_EVENT_OR);
    break;

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
