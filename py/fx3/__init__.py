import time
import nitro
from nitro_parts.Microchip.M24XX import program_fx3_prom 
import logging, numpy, struct
log=logging.getLogger(__name__)


def get_dev(di_file="Cypress/fx3/fx3.xml", serial_num=None, bus_addr=None, VID=0x1fe1, PID=0x00F0, timeout=10):
    """Opens a device and loads the DI file. If a serial_num is
       provided, this opens that device. You can also specify the VID
       and PID. This will wait until 'timeout' seconds for a device to
       connect."""

    dev = nitro.USBDevice(VID, PID)
    
    time0 = time.time()
    while time0+timeout > time.time():
        try:
            if not(bus_addr is None):
                dev.open_by_address(bus_addr)
                break
            elif not(serial_num is None):
                dev.open_by_serial(serial_num)
                break
            else:
                n=nitro.USBDevice.get_device_count(VID,PID)
                if n<0:
                    log.info("Waiting for device with VID=0x%x PID=0x%x to connect" % (VID,PID))
                    time.sleep(1)
                    continue
                log.info("Found %d devices on bus with VID=0x%04x PID=0x%04x" % (n,VID,PID))
                idx=0
                for vid,pid,addr in dev.get_device_list(VID,PID):
                    try:
                        log.info("Trying to open device %d: ADDR=%d." % (idx, addr))
                        dev.open_by_address(addr)
                        break
                    except nitro.Exception as e:
                        log.info("Unable to open device %d. ADDR=%d. %s" % (idx, addr, e))
                    idx += 1
            if dev.is_open():
                break
            log.info("Unable to open any devices on bus.\n")
            time.sleep(1)
            continue
        except nitro.Exception:
            if not(bus_addr is None):
                log.info("Waiting for device with VID=0x%x PID=0x%x BUS_ADDR=%d to connect" % (VID,PID,bus_addr))
            elif not(serial_num is None):
                log.info("Waiting for device with VID=0x%x PID=0x%x SERIAL_NUM=%s to connect" % (VID,PID,serial_num))
            else:
                log.exception("Error opening device.")
                break
            time.sleep(1)
                
    if not(dev.is_open()):
        raise nitro.Exception("Could not open 0x%x:0x%x device" % (VID, PID))
    try:
        serial = dev.get_serial()
        if serial==u'\uffff'*8:
            serial='uninitialized'
    except:
        serial = "UNKNOWN"
    log.info("Opened 0x%x:0x%x device with serial number %s" % (VID, PID, serial))

    if(di_file):
        init_dev(dev, di_file)

    return dev

def init_dev(dev, di_file="Cypress/fx3/fx3.xml"):
    """Initializes the device with the di files."""

    di = nitro.load_di(di_file)
    dev.set_di(di)

def program_fx3(dev, filename):
    """
        Program the fx3 with the firmware ihx file.  filename should be set to
        the path of the ihx file.  This function causes the fx3 to reboot and
        renumerate with the USB bus.  The device is automatically closed and 
        must be re-opened after it has reconnected to the Host.
    """
    log.info("Programming FX3 with img file %s" % filename)
    f=open(filename,'rb').read()
#    print(type(dev))
#    dev=nitro.USBDevice(dev) # ensure load_firmware available
    dev.load_firmware ( f )
    log.info ("Firmware loaded, device automatically closed." )

def program_new_pcb(fx3_firmware, VID, PID, di_file, fx3_prom_term='FX3_PROM'):
    """
        This function does not require an open device.  It looks
        for the 1st unprogrammed pcb (by using the default Cypress
        Vendor ID/Product ID and attempts to load the fx3 firmware
        files specified.

        :param fx3_firmware: The fx3 firmare ihx file. 
        :param vendor id of firmware being loaded.
        :param product id of firmware being loaded.
        :param path to di file implemented by firmware.
    """
    dev=nitro.USBDevice(0x04b4,0x00f3)
    dev.open(0,True)
    program_fx3(dev,fx3_firmware)
    time.sleep(1)

    # 
    while nitro.USBDevice.get_device_count(VID, PID) < 1:
        time.sleep(1)

    dev=nitro.USBDevice(VID, PID)
    dev.open()
    dev.set_di( nitro.load_di ( di_file ) )
    program_fx3_prom(dev, fx3_firmware, fx3_prom_term)
    dev.close()


def read_log(dev):
    c=dev.get('LOG','count')
    if c:
        buf=numpy.zeros(c,dtype=numpy.uint8)
        dev.read('LOG','log',buf)
        while len(buf):
            #print buf[0], # level
            buf=buf[1:]
            i=numpy.where(buf == 0)[0][0]
            print(struct.unpack("%ds" % i, buf[:i])[0],)
            buf=buf[i+1:]


