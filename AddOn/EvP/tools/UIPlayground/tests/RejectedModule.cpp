#define TAPIOCA_UI_MODULE 1
#include "UiAbi.h"

uint32_t TapiocaUiAbiVersion (void)
{
    return TAPIOCA_UI_ABI_VERSION + 1;
}
void* TapiocaUiCreate (const TapiocaUiHost*, const char*)
{
    return nullptr;
}
void TapiocaUiDestroy (void*)
{
}
int TapiocaUiDraw (void*, const TapiocaUiFrame*, TapiocaUiResult*)
{
    return 0;
}
