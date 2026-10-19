#include "GeometryRustFfi.h"

#include <stdio.h>
#include <string.h>

int TapiocaCppHeaderCheck (void);

int main (void)
{
    uint32_t version = 99;
    char error[128] = "not cleared";
    if (sizeof (TapiocaStatus) != 4 || tapioca_version (&version, error, sizeof (error)) != TAPIOCA_OK ||
        version != TAPIOCA_ABI_VERSION || error[0] != '\0')
        return 1;

    if (tapioca_version (NULL, error, sizeof (error)) != TAPIOCA_ERR_ARG ||
        strstr (error, "Null or misaligned") == NULL)
        return 2;

    version = 99;
    if (tapioca_version (&version, NULL, 1) != TAPIOCA_ERR_ARG || version != 99)
        return 3;
    if (tapioca_version (&version, NULL, 0) != TAPIOCA_OK || version != TAPIOCA_ABI_VERSION)
        return 4;
    if (TapiocaCppHeaderCheck () != 0)
        return 5;

    printf ("Tapioca aggregate ABI v%u: C and C++ link/status checks passed\n", (unsigned) version);
    return 0;
}
