#define CINTERFACE
#include <windows.h>
#include <dinput.h>
#include <d3d9.h>
#include <stdio.h>
#include <stddef.h>

#define IDX(structname, member) (offsetof(structname##Vtbl, member) / sizeof(void*))

int main(void)
{
    printf("di8  CreateDevice      = %d\n", IDX(IDirectInput8W, CreateDevice));
    printf("dev  SetProperty        = %d\n", IDX(IDirectInputDevice8W, SetProperty));
    printf("dev  Acquire            = %d\n", IDX(IDirectInputDevice8W, Acquire));
    printf("dev  Unacquire          = %d\n", IDX(IDirectInputDevice8W, Unacquire));
    printf("dev  GetDeviceState     = %d\n", IDX(IDirectInputDevice8W, GetDeviceState));
    printf("dev  GetDeviceData      = %d\n", IDX(IDirectInputDevice8W, GetDeviceData));
    printf("dev  SetDataFormat      = %d\n", IDX(IDirectInputDevice8W, SetDataFormat));
    printf("d3d  CreateDevice       = %d\n", IDX(IDirect3D9, CreateDevice));
    printf("d3ddev Reset            = %d\n", IDX(IDirect3DDevice9, Reset));
    printf("d3ddev Present          = %d\n", IDX(IDirect3DDevice9, Present));
    printf("d3ddev DrawIndexedPrim  = %d\n", IDX(IDirect3DDevice9, DrawIndexedPrimitive));
    return 0;
}
