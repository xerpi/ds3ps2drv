#include <thbase.h>
#include <sifcmd.h>
#include <usbd.h>
#include <usbd_macro.h>
#include <string.h>
#include "ds3ps2drv.h"

static struct SS_GAMEPAD gamepad[DS3PS2_MAX_SLOTS];
static u8 opbuf[17] __attribute((aligned(64)));
static int controlEndp;

static int usb_probe(int devId);
static int usb_connect(int devId);
static int usb_disconnect(int devId);

static UsbDriver driver = { NULL, NULL, "ds3ps2", usb_probe, usb_connect, usb_disconnect };

static void request_data(int result, int count, void *arg);
static void config_set(int result, int count, void *arg);
static void ds3_set_operational(int slot);

static struct {
    int devID;
    int connected;
    int endp;
    int led;
    struct {
        int time_r, power_r;
        int time_l, power_l;
    } rumble;
} ds3_list[DS3PS2_MAX_SLOTS];

int _start()
{
    UsbRegisterDriver(&driver);
    return 1;
}

int usb_probe(int devId)
{
    UsbDeviceDescriptor *dev = NULL;
    dev = UsbGetDeviceStaticDescriptor(devId, NULL, USB_DT_DEVICE);
    if (!dev)
        return 0;
    
    if (dev->idVendor == DS3_VID && dev->idProduct == DS3_PID) {
        //Check if there's an available slot
        if (ds3_list[0].connected && ds3_list[1].connected) return 0;
        return 1;
    }
    
    return 0;
}

int usb_connect(int devId)
{
    UsbDeviceDescriptor *dev;
    UsbConfigDescriptor *conf;
    
    dev = UsbGetDeviceStaticDescriptor(devId, NULL, USB_DT_DEVICE);
    conf = UsbGetDeviceStaticDescriptor(devId, dev, USB_DT_CONFIG);
    controlEndp = UsbOpenEndpoint(devId, NULL);
    
    int slot = 0;
    if (ds3_list[0].connected) slot = 1;
    ds3_list[slot].endp = controlEndp;
    ds3_list[slot].connected = 1;
    ds3_list[slot].devID = devId;

    UsbSetDevicePrivateData(devId, NULL);
    UsbSetDeviceConfiguration(controlEndp, conf->bConfigurationValue, config_set, (void*)slot);
    return 0;
}

int usb_disconnect(int devId)
{
    if (devId == ds3_list[0].devID) ds3_list[0].connected = 0;
    else ds3_list[1].connected = 0;
    return 1;
}

static void config_set(int result, int count, void *arg)
{
    int slot = (int)arg;
    //Set operational
    ds3_set_operational(slot);
    //Set LED
    ds3ps2_set_led(slot, slot+1);
    ds3ps2_send_ledsrumble(slot);
    //Start reading!
    request_data(0, 0, (void *)slot);
}

#define INTERFACE_GET (USB_DIR_IN|USB_TYPE_CLASS|USB_RECIP_INTERFACE)
#define INTERFACE_SET (USB_DIR_OUT|USB_TYPE_CLASS|USB_RECIP_INTERFACE)
#define USB_REPTYPE_INPUT       0x01
#define USB_REPTYPE_OUTPUT      0x02
#define USB_REPTYPE_FEATURE     0x03


#define swap16(x) (((x&0xFF)<<8)|((x>>8)&0xFF))
#define zeroG 511.5
static void correct_data(struct SS_GAMEPAD *data)
{
    data->motion.acc_x = swap16(data->motion.acc_x) - zeroG;
    data->motion.acc_y = swap16(data->motion.acc_y) - zeroG;
    data->motion.acc_z = swap16(data->motion.acc_z) - zeroG;
    data->motion.z_gyro = swap16(data->motion.z_gyro) - zeroG;
}

static void request_data_cb(int result, int count, void *arg)
{
    int slot = (int)arg;
    correct_data(&gamepad[slot]);
    request_data(0, 0, (void *)slot);
}


static void request_data(int result, int count, void *arg)
{
    int slot = (int)arg;
    UsbControlTransfer(ds3_list[slot].endp,
        INTERFACE_GET,
        USB_REQ_GET_REPORT,
        (USB_REPTYPE_INPUT<<8) | 0x01,
        0x0,
        DS3PS2_INPUT_LEN,
        &gamepad[slot],
        request_data_cb,
        arg);
}

static void ds3_set_operational(int slot)
{
    UsbControlTransfer(ds3_list[slot].endp,
        INTERFACE_GET,
        USB_REQ_GET_REPORT,
        (USB_REPTYPE_FEATURE<<8) | 0xf2,
        0x0,
        17,
        opbuf,
        NULL, NULL);
}

static const u8 led_pattern[] = {0x0, 0x02, 0x04, 0x08, 0x10, 0x12, 0x14, 0x18};
static u8 __attribute__((aligned(64))) ledsrumble_buf[] =
{
    0x52,
    0x00, 0x00, 0x00, 0x00, //Rumble
    0xff, 0x80,             //Gyro
    0x00, 0x00,
    0x02, //* LED_1 = 0x02, LED_2 = 0x04, ... */
    0xff, 0x27, 0x10, 0x00, 0x32, /* LED_4 */
    0xff, 0x27, 0x10, 0x00, 0x32, /* LED_3 */
    0xff, 0x27, 0x10, 0x00, 0x32, /* LED_2 */
    0xff, 0x27, 0x10, 0x00, 0x32, /* LED_1 */
};

int ds3ps2_slot_connected(int slot)
{
    return ds3_list[slot].connected;
}

void ds3ps2_set_led(int slot, u8 n)
{
    ds3_list[slot].led = n;
}

void ds3ps2_set_rumble(int slot, u8 power_r, u8 time_r, u8 power_l, u8 time_l)
{
    ds3_list[slot].rumble.time_r = time_r;
    ds3_list[slot].rumble.power_r = power_r;
    ds3_list[slot].rumble.time_l = time_l;
    ds3_list[slot].rumble.power_l = power_l;
}

int ds3ps2_send_ledsrumble(int slot)
{
    ledsrumble_buf[9] = led_pattern[ds3_list[slot].led];
    ledsrumble_buf[1] = ds3_list[slot].rumble.time_r;
    ledsrumble_buf[2] = ds3_list[slot].rumble.power_r;
    ledsrumble_buf[3] = ds3_list[slot].rumble.time_l;
    ledsrumble_buf[4] = ds3_list[slot].rumble.power_l;
    
    return UsbControlTransfer(ds3_list[slot].endp,
        INTERFACE_SET,
        USB_REQ_SET_REPORT,
        (USB_REPTYPE_OUTPUT<<8) | 0x01,
        0x0,
        sizeof(ledsrumble_buf),
        ledsrumble_buf,
        NULL, NULL);  
}

void ds3ps2_get_input(int slot, struct SS_GAMEPAD *ps3pad)
{
    memcpy(ps3pad, &gamepad[slot],  sizeof(struct SS_GAMEPAD));
}
