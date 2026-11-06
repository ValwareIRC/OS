#pragma once
#include <stdbool.h>
#include <stdint.h>

/* USB (drivers/usb): the xHCI host controller every computer of the last decade has (xhci.c), the devices on it
 * and on hubs, and the class drivers that use them: keyboards and mice (hid.c), sticks and drives (msc.c).
 *
 * One kernel thread per controller owns it: it finds what is plugged in (and what goes), and carries the
 * class drivers' repeating transfers. Other threads (a disk read) may make transfers too: every use of a
 * controller holds its lock, and a transfer waits for its end by working the controller's event ring itself. */

#define USB_DIR_IN 0x80
#define USB_TYPE_STANDARD (0 << 5)
#define USB_TYPE_CLASS (1 << 5)
#define USB_RECIP_DEVICE 0
#define USB_RECIP_INTERFACE 1
#define USB_RECIP_ENDPOINT 2
#define USB_RECIP_OTHER 3

#define USB_REQ_GET_STATUS 0
#define USB_REQ_CLEAR_FEATURE 1
#define USB_REQ_SET_FEATURE 3
#define USB_REQ_GET_DESCRIPTOR 6
#define USB_REQ_SET_CONFIGURATION 9
#define USB_REQ_SET_INTERFACE 11

#define USB_DT_DEVICE 1
#define USB_DT_CONFIG 2
#define USB_DT_STRING 3
#define USB_DT_INTERFACE 4
#define USB_DT_ENDPOINT 5
#define USB_DT_HID 0x21
#define USB_DT_REPORT 0x22
#define USB_DT_HUB 0x29
#define USB_DT_SS_HUB 0x2a

#define USB_CLASS_HID 3
#define USB_CLASS_MASS_STORAGE 8
#define USB_CLASS_HUB 9

enum usb_speed { USB_SPEED_FULL = 1, USB_SPEED_LOW = 2, USB_SPEED_HIGH = 3, USB_SPEED_SUPER = 4, USB_SPEED_SUPER_PLUS = 5 };

struct usb_endpoint {
    uint8_t address;    /* number, | USB_DIR_IN */
    uint8_t attributes; /* 2 bulk, 3 interrupt */
    uint16_t max_packet;
    uint8_t interval;
    uint8_t max_burst; /* (SuperSpeed companion) */
};

struct usb_interface {
    uint8_t number, alt, cls, subclass, protocol;
    int nendpoints;
    struct usb_endpoint ep[8];
    const uint8_t *extra; /* the class descriptors that follow it (HID's) */
    int extra_len;
};

struct xhci;
struct usb_driver;

struct usb_device {
    struct xhci *hc;
    int slot;
    enum usb_speed speed;
    int root_port;       /* 1-based, on the controller */
    uint32_t route;      /* the hub ports below the root, four bits each */
    struct usb_device *parent; /* a hub, or NULL */
    int parent_port;
    int depth;
    uint16_t vendor, product;
    uint8_t cls, subclass, protocol;
    char name[64];
    uint8_t *config; /* the whole configuration descriptor */
    int config_len;
    int ninterfaces;
    struct usb_interface intf[8];
    struct usb_driver *drv[8]; /* per interface */
    void *drv_data[8];
    bool gone;           /* unplugged: transfers fail at once */
    /* (controller state) */
    void *hcd;           /* the controller driver's own */
    void *rings[32];     /* by endpoint context index */
    uint64_t out_ctx_phys;
    void *out_ctx;
    struct usb_device *next;
    /* a hub's own */
    int hub_ports;
    bool is_hub, hub_tt_multi;
};

/* A class driver: probe is offered each interface of each new device and returns true if it takes it. */
struct usb_driver {
    const char *name;
    bool (*probe)(struct usb_device *d, int intf);
    void (*disconnect)(struct usb_device *d, int intf);
    struct usb_driver *next;
};

void usb_init(void); /* after pci_init and input_init: finds every xHCI controller */
void usb_register_driver(struct usb_driver *drv);

/* A control transfer on endpoint 0: bytes moved, or a negative errno (-EPIPE: the device stalled it). */
int usb_control(struct usb_device *d, uint8_t reqtype, uint8_t req, uint16_t value, uint16_t index, void *data,
                uint16_t len);
/* Readies an endpoint of an interface for transfers. */
int usb_enable_endpoint(struct usb_device *d, const struct usb_endpoint *ep);
/* A bulk or interrupt transfer, waited for (timeout_ms 0: forever): bytes moved, or a negative errno. */
int usb_transfer(struct usb_device *d, uint8_t ep_addr, void *data, uint32_t len, int timeout_ms);
/* An endpoint unstalled (CLEAR_FEATURE ENDPOINT_HALT, and the controller's side reset). */
int usb_clear_halt(struct usb_device *d, uint8_t ep_addr);
/* An interrupt IN endpoint read over and over by the controller's thread: done(...) with each report. */
int usb_poll_endpoint(struct usb_device *d, uint8_t ep_addr, uint16_t len,
                      void (*done)(struct usb_device *d, void *arg, const uint8_t *data, int len), void *arg);
