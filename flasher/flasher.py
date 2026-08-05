#!/usr/bin/env python3
import os
import sys
import argparse
import gpiod
from gpiod.line import Direction, Value

#define chip e pino
CHIP_PATH = "/dev/gpiochip0"
PIN_OFFSET = 17 #alterar depois se necessario

_pin_req = None #para requisitar pino

def init_pin(chip_path=CHIP_PATH, pin=PIN_OFFSET):
    global _pin_req
    
    if _pin_req is not None:
        return

    _pin_req = gpiod.request_lines(
        chip_path,
        consumer="mmwave-controller",
        config={
            pin: gpiod.LineSettings(
                direction=Direction.OUTPUT,
                output_value=Value.INACTIVE
            )
        }
    )

def flash_pin_on(pin=PIN_OFFSET):
    _pin_req.set_value(pin, Value.ACTIVE)

def flash_pin_off(pin=PIN_OFFSET):
    _pin_req.set_value(pin, Value.INACTIVE)

def end_pin(): #libera pino
    global _pin_req
    
    if _pin_req is not None:
        _pin_req.close() 
        _pin_req = None

# Add search paths for local modules
dir_path = os.path.dirname(os.path.realpath(__file__))
sys.path.append(dir_path)

try:
    import mmWaveProgFlash
except ImportError:
    print("\033[91m[ERROR] Could not find 'mmWaveProgFlash.py' in the current directory.\033[0m")
    sys.exit(1)

# Default configuration settings
TIMEOUT_VALUE = 1
DEFAULT_BAUDRATE = 115200

FP_TRACE_LEVEL_FATAL = 3
FP_TRACE_LEVEL_ERROR = 2
FP_TRACE_LEVEL_WARNING = 1
FP_TRACE_LEVEL_INFO = 0
FP_TRACE_LEVEL_DEBUG = 255

# Simple class mimicking the image objects passed by the framework
class ImageObject:
    def __init__(self, path, order=1):
        self.path = path
        self.order = order  # Standard order 1 corresponds to META_IMAGE1
        self.file_id = ""   # To be populated by checkFileHeader
        self.fileSize = 0   # To be populated by checkFileHeader


# Main Flasher class stripped of desktop GUI framework bindings
class FlashPython:

    def __init__(self):
        # Notify user of initialization
        self.update_progress("Initialization of uniflash object completed", 0)
        
        self.push_message(f"Starting {mmWaveProgFlash.BootLdr.__name__}", FP_TRACE_LEVEL_DEBUG)
        
        # Instantiate the TI BootLdr
        # We pass 'self' so the bootloader can redirect its status updates back here
        self.ldr = mmWaveProgFlash.BootLdr(self, 'COM1')
        
        self.update_progress("Initialization complete.", 1)
        self.push_message("Initialization complete.", FP_TRACE_LEVEL_INFO)
        self.ldr.update_prog_percentage(1)

    # Concrete CLI implementation of UI progress updates
    def update_progress(self, message, percentage):
        print(f"[{percentage:3d}%] {message}")

    # Concrete CLI implementation of logging levels
    def push_message(self, message, level):
        if level == FP_TRACE_LEVEL_FATAL:
            print(f"\033[91m[FATAL] {message}\033[0m")
        elif level == FP_TRACE_LEVEL_ERROR:
            print(f"\033[91m[ERROR] {message}\033[0m")
        elif level == FP_TRACE_LEVEL_WARNING:
            print(f"\033[93m[WARNING] {message}\033[0m")
        elif level == FP_TRACE_LEVEL_INFO:
            print(f"[INFO] {message}")
        elif level == FP_TRACE_LEVEL_DEBUG:
            print(f"[DEBUG] {message}")

    def check_is_cancel_set(self):
        return False  # No manual cancellation via CLI

    def load_image(self, images, propertiesMap):
        self.images = images
        status = True
        self.propertiesMap = propertiesMap
        
        # Verify target parameters
        status = self.ldr.checkPropertiesMapKeys(propertiesMap)
        if status is False:
            self.push_message("Check Integration. Dict keys do not match", FP_TRACE_LEVEL_FATAL)
            return

        c = self.check_is_cancel_set()
        if c is False:
            self.push_message("Flashing process starting...", FP_TRACE_LEVEL_INFO)
            self.com_port = self.propertiesMap['COMPort']
            
            # Establish device connection
            passed = self.ldr.connect(TIMEOUT_VALUE, self.com_port)
            if passed is True:
                storage = self.propertiesMap['MemSelectRadio']
                partNum = self.propertiesMap['partnum']
                
                if self.ldr.isPartNumSupported(partNum):
                    self.push_message(f"Flashing files to {partNum} device.", FP_TRACE_LEVEL_DEBUG)
                    self.ldr.setPartNum(partNum)
                    
                    # Interrogate bootloader version (Gen1 vs Gen3 logic)
                    status = self.ldr.determinePGVersion()
                    if status is False:
                        self.push_message("Not able to get version of device. Please power cycle device and try again.", FP_TRACE_LEVEL_ERROR)
                        self.ldr.disconnect()
                        return
                    
                    filesList = self.ldr.copyImagesList(images)
                    
                    # Verify binary headers (Checking for 0x5254534D / 'RTSM' for IWR6843)
                    fileSizeSum = 0
                    correctFile = True
                    self.push_message(f"** {len(filesList)} files specified for flashing.", FP_TRACE_LEVEL_INFO)
                    
                    for i in filesList:
                        correctFile = self.ldr.checkFileHeader(i.path, i)
                        if correctFile is False:
                            break
                        else:
                            fileSizeSum = fileSizeSum + i.fileSize
                            
                    if correctFile is False:
                        self.update_progress("!!! Aborting flashing of specified files!!!", 90)
                        self.push_message("!!! Aborting flashing of specified files!!!", FP_TRACE_LEVEL_FATAL)
                        self.ldr.disconnect()
                        return
                    
                    self.push_message(f"!! Files are valid for {partNum}.", FP_TRACE_LEVEL_INFO)
                    
                    # Calculate tracking milestones
                    self.ldr.calcProgressValues(filesList, fileSizeSum, self.propertiesMap['DownloadFormat'])
                    
                    if self.check_is_cancel_set():
                        self.push_message(mmWaveProgFlash.AWR_CANCEL_MSG, FP_TRACE_LEVEL_WARNING)
                        self.ldr.disconnect()
                        return
                    
                    # Execute format if requested
                    if self.propertiesMap['DownloadFormat'] == True:
                        nextPercentage = self.ldr.get_prog_percentage() + 1
                        self.push_message("Format on download was specified. Formatting SFLASH storage...", FP_TRACE_LEVEL_INFO)
                        self.update_progress("Format before download started ...", nextPercentage)
                        self.ldr.update_prog_percentage(nextPercentage)
                        self.ldr.erase_storage()
                        nextPercentage = self.ldr.get_prog_percentage() + 1
                        self.update_progress("Format Complete!", nextPercentage)
                        self.ldr.update_prog_percentage(nextPercentage)
                    
                    # Flash target binary files
                    for i in filesList:
                        imageProgCntList = self.ldr.getImageProgCntList(i)
                        # Write the payload through the serial bootloader
                        status = self.ldr.download_file(i.path, i.file_id, 0, 0, storage, imageProgCntList)
                        if status is False:
                            self.push_message("FAILURE: File Download Failure!! Ceasing session...", FP_TRACE_LEVEL_FATAL)
                            self.push_message("Aborting operations...", FP_TRACE_LEVEL_FATAL)
                            break
                        else:
                            self.push_message(f"SUCCESS!! File type {i.file_id} downloaded successfully to {storage}.", FP_TRACE_LEVEL_INFO)
                            
                        if self.check_is_cancel_set():
                            self.ldr.disconnect()
                            self.push_message(mmWaveProgFlash.AWR_CANCEL_MSG, FP_TRACE_LEVEL_WARNING)
                            break
                    
                    self.ldr.disconnect()
                else:
                    self.push_message("Internal Error: Unknown device type. Cannot proceed. Exiting...", FP_TRACE_LEVEL_FATAL)
                    self.update_progress("Cannot proceed", 90)
            else:
                self.push_message("Not able to connect to serial port. Recheck COM port selected and/or permissions.", FP_TRACE_LEVEL_FATAL)
        else:
            self.push_message(mmWaveProgFlash.AWR_CANCEL_MSG, FP_TRACE_LEVEL_WARNING)


# Command-line Execution Interface
if __name__ == "__main__":

    init_pin()
    flash_pin_off()

    parser = argparse.ArgumentParser(description="Standalone CLI Firmware Flasher for Gen1 TI mmWave Radar Devices")
    parser.add_argument("-p", "--port", required=True, help="Target serial port (e.g., /dev/ttyUSB0 or COM4)")
    parser.add_argument("-f", "--file", required=True, help="Path to firmware binary (.bin file)")
    parser.add_argument("-d", "--device", default="IWR6843", help="Target device variant name (default: IWR6843)")
    parser.add_argument("-e", "--erase", action="store_true", help="Erase/Format SFLASH storage area prior to flashing")
    
    args = parser.parse_args()

    # Normalize device part string to fit target criteria (e.g., "IWR68")
    part_num = args.device
    if part_num == "IWR6843":
        part_num = "IWR68xx"  # Matches the xWR68xx part prefix expectations

    if not os.path.exists(args.file):
        print(f"\033[91m[ERROR] Firmware file not found: {args.file}\033[0m")
        sys.exit(1)

    print("==================================================")
    print("      TI mmWave Radar Command-Line Flasher        ")
    print("==================================================")
    print(f" Port:    {args.port}")
    print(f" File:    {args.file}")
    print(f" Device:  {part_num}")
    print(f" Format:  {args.erase}")
    print("--------------------------------------------------")
    print("Please set your device to SOP5 Flashing Mode and reset/repower.")
    print("================================================--")

    # Build standard properties layout dictionary
    props = {
        'COMPort': args.port,
        'MemSelectRadio': 'SFLASH',
        'partnum': part_num,
        'DownloadFormat': args.erase
    }

    flash_pin_on()

    # Instantiate the adjusted runner and execute the flash sequence
    flasher = FlashPython()
    image_queue = [ImageObject(args.file, order=1)] # Point to default image slot (META_IMAGE1)
    flasher.load_image(image_queue, props)

    flash_pin_off()
    end_pin()

