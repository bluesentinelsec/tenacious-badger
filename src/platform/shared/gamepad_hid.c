/* The drivers of HID pads that the raw HID backends know. A pad that an
 * XInput or GameController backend reads already needs none. */
#include "gamepad_hid.h"

#include "gamepad_switch2.h"

static const tb_hid_driver *const tb_hid_drivers[] = {
    &tb_switch2_driver,
};

const tb_hid_driver *tb_hid_driver_find(uint16_t vendor, uint16_t product)
{
    for (size_t index = 0; index < sizeof(tb_hid_drivers) / sizeof(tb_hid_drivers[0]); index++)
    {
        if (tb_hid_drivers[index]->matches(vendor, product))
        {
            return tb_hid_drivers[index];
        }
    }
    return NULL;
}
