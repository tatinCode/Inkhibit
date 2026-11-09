#ifndef INKHIBIT_TEST_LGPIO_H
#define INKHIBIT_TEST_LGPIO_H

#ifdef __cplusplus
extern "C" {
#endif

/* Only the lgpio API used by Inkhibit's adapter is substituted. */
int lgGpiochipOpen(int gpio_dev);
int lgGpiochipClose(int handle);
int lgGpioClaimInput(int handle, int flags, int gpio);
int lgGpioClaimOutput(int handle, int flags, int gpio, int level);
int lgGpioFree(int handle, int gpio);
int lgGpioRead(int handle, int gpio);
int lgGpioWrite(int handle, int gpio, int level);
int lgSpiOpen(int device, int channel, int baud, int flags);
int lgSpiClose(int handle);
int lgSpiWrite(int handle, const char *data, int count);
const char *lguErrorText(int error);

#ifdef __cplusplus
}
#endif

#endif
