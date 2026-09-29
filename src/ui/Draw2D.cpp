#include "Draw2D.h"

#include <string.h>

namespace
{
    struct Vertex
    {
        float x, y, z, rhw;
        DWORD color;
        float u, v;
    };
    const DWORD VERTEX_FVF = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1;

    // Textured draws take texture x colour, untextured ones the colour alone.
    // With no texture bound, what D3DTA_TEXTURE reads is up to the driver, so
    // the two paths are set apart explicitly.
    void UseTexture(IDirect3DDevice9* device, IDirect3DTexture9* texture)
    {
        device->SetTexture(0, texture);
        if (texture)
        {
            device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
            device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
        }
        else
        {
            device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
            device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
        }
    }
}

namespace Draw2D
{
    void Begin(IDirect3DDevice9* device)
    {
        device->SetVertexShader(NULL);
        device->SetPixelShader(NULL);
        device->SetFVF(VERTEX_FVF);

        device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        // Premultiplied, hence SRCBLEND = ONE.
        device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
        device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        device->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
        device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        device->SetRenderState(D3DRS_ZENABLE, FALSE);
        device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        device->SetRenderState(D3DRS_LIGHTING, FALSE);
        device->SetRenderState(D3DRS_FOGENABLE, FALSE);
        device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        device->SetRenderState(D3DRS_COLORWRITEENABLE, 0x0F);
        device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);

        device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
        device->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
        device->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
        device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        device->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);

        device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    }

    DWORD Premul(DWORD argb, float opacity)
    {
        if (opacity < 0.0f) opacity = 0.0f;
        if (opacity > 1.0f) opacity = 1.0f;
        float a = ((argb >> 24) & 0xFF) / 255.0f * opacity;
        DWORD A = (DWORD)(a * 255.0f + 0.5f);
        DWORD R = (DWORD)(((argb >> 16) & 0xFF) * a + 0.5f);
        DWORD G = (DWORD)(((argb >> 8) & 0xFF) * a + 0.5f);
        DWORD B = (DWORD)((argb & 0xFF) * a + 0.5f);
        return (A << 24) | (R << 16) | (G << 8) | B;
    }

    void Image(IDirect3DDevice9* device, IDirect3DTexture9* texture,
               float x, float y, float w, float h, DWORD argb, float opacity)
    {
        if (!texture) return;
        const DWORD c = Premul(argb, opacity);
        // -0.5 aligns texel to pixel in D3D9.
        x -= 0.5f;
        y -= 0.5f;
        Vertex v[4] = {
            { x,     y,     0.0f, 1.0f, c, 0.0f, 0.0f },
            { x + w, y,     0.0f, 1.0f, c, 1.0f, 0.0f },
            { x,     y + h, 0.0f, 1.0f, c, 0.0f, 1.0f },
            { x + w, y + h, 0.0f, 1.0f, c, 1.0f, 1.0f },
        };
        UseTexture(device, texture);
        device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(Vertex));
    }

    void Polygon(IDirect3DDevice9* device, const float* xy, const DWORD* argb,
                 int n, float opacity)
    {
        if (n < 3 || n > 16) return;
        Vertex v[16];
        for (int i = 0; i < n; i++)
        {
            Vertex p = { xy[i * 2] - 0.5f, xy[i * 2 + 1] - 0.5f, 0.0f, 1.0f,
                         Premul(argb[i], opacity), 0.0f, 0.0f };
            v[i] = p;
        }
        UseTexture(device, NULL);
        device->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, n - 2, v, sizeof(Vertex));
    }

    void Rect(IDirect3DDevice9* device, float x, float y, float w, float h,
              DWORD left, DWORD right, float opacity)
    {
        const float xy[8] = { x, y,  x + w, y,  x + w, y + h,  x, y + h };
        const DWORD c[4] = { left, right, right, left };
        Polygon(device, xy, c, 4, opacity);
    }

    IDirect3DTexture9* Texture(IDirect3DDevice9* device, const void* bgra,
                               int width, int height)
    {
        IDirect3DTexture9* texture = 0;
        if (FAILED(device->CreateTexture(width, height, 1, 0, D3DFMT_A8R8G8B8,
                                         D3DPOOL_MANAGED, &texture, NULL)))
            return 0;

        D3DLOCKED_RECT lr;
        if (FAILED(texture->LockRect(0, &lr, NULL, 0)))
        {
            texture->Release();
            return 0;
        }
        const size_t row = (size_t)width * 4;
        for (int y = 0; y < height; y++)
            memcpy((unsigned char*)lr.pBits + (size_t)y * lr.Pitch,
                   (const unsigned char*)bgra + (size_t)y * row, row);
        texture->UnlockRect(0);
        return texture;
    }
}
