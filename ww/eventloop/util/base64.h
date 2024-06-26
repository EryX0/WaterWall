#ifndef HV_BASE64_H_
#define HV_BASE64_H_

#include "hexport.h"

#define BASE64_ENCODE_OUT_SIZE(s)   (((s) + 2) / 3 * 4)
#define BASE64_DECODE_OUT_SIZE(s)   (((s)) / 4 * 3)

BEGIN_EXTERN_C

// @return encoded size
HV_EXPORT int hv_base64_encode(const unsigned char *in, unsigned int inlen, char *out);

// @return decoded size
HV_EXPORT int hv_base64_decode(const char *in, unsigned int inlen, unsigned char *out);

END_EXTERN_C


#endif // HV_BASE64_H_
