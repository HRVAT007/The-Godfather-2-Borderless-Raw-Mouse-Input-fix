#include <windows.h>
#include <d3d9.h>
#include <stdio.h>

template <typename PMF>
static int VIdx(PMF pmf)
{
    union { PMF p; DWORD d[2]; } u;
    u.p = pmf; u.d[1] = 0;
    return (int)(u.d[0] / sizeof(void*));
}

int main()
{
    printf("Reset=%d\n", VIdx(&IDirect3DDevice9::Reset));
    printf("Present=%d\n", VIdx(&IDirect3DDevice9::Present));
    printf("BeginScene=%d\n", VIdx(&IDirect3DDevice9::BeginScene));
    printf("EndScene=%d\n", VIdx(&IDirect3DDevice9::EndScene));
    printf("DrawPrimitive=%d\n", VIdx(&IDirect3DDevice9::DrawPrimitive));
    printf("DrawIndexedPrimitive=%d\n", VIdx(&IDirect3DDevice9::DrawIndexedPrimitive));
    printf("DrawPrimitiveUP=%d\n", VIdx(&IDirect3DDevice9::DrawPrimitiveUP));
    printf("DrawIndexedPrimitiveUP=%d\n", VIdx(&IDirect3DDevice9::DrawIndexedPrimitiveUP));
    printf("GetFVF=%d\n", VIdx(&IDirect3DDevice9::GetFVF));
    printf("GetStreamSource=%d\n", VIdx(&IDirect3DDevice9::GetStreamSource));
    printf("CreateDevice(d3d9)=%d\n", VIdx(&IDirect3D9::CreateDevice));
    return 0;
}
