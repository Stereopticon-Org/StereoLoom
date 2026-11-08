// OutputEncoder.h -- the two eyes drawn for a display that is not a Leia SR one:
// anaglyph for coloured glasses, side by side or top and bottom for a 3D TV (or a
// Zondision display, which weaves side by side itself), and row / column /
// checkerboard interleaving for passive and DLP 3D displays. SRWeaver uses it in
// place of the SR weave, from the same side-by-side picture, into the same back
// buffer: everything before it (capture, conversion, look-around) is unchanged.
#pragma once

#include "Common.h"
#include <d3d11.h>

namespace srw
{
    class OutputEncoder
    {
    public:
        OutputEncoder() = default;
        ~OutputEncoder() { Release(); }

        bool Create(ID3D11DeviceContext* context);
        void Release();
        bool IsReady() const { return m_ps != nullptr; }

        // How: the output (any but SR / Zondision's own: Zondision is drawn as side by
        // side), the anaglyph's colours and style (Settings OutputAnaColours /
        // OutputAnaStyle) and the eyes swapped.
        void SetOptions(DisplayOutput output, int anaColours, int anaStyle, bool swapEyes);
        // The side-by-side picture (left | right, each perEyeWidth wide) to draw from.
        void SetInput(ID3D11ShaderResourceView* srv, int perEyeWidth, int height);
        int EyeWidth() const { return m_eyeW; }
        int EyeHeight() const { return m_h; }
        // Drawn into the bound render target, over the viewport set (the renderer's).
        // originX / originY: where the render target's pixel (0, 0) is on its
        // display, in pixels from the display's top left -- so the interleaved rows
        // and columns line up with the display's own.
        void Draw(int originX, int originY);

    private:
        void Fill();

        ID3D11DeviceContext*      m_ctx = nullptr;   // not owned
        ID3D11VertexShader*       m_vs  = nullptr;
        ID3D11PixelShader*        m_ps  = nullptr;
        ID3D11Buffer*             m_cb  = nullptr;
        ID3D11SamplerState*       m_smp = nullptr;
        ID3D11ShaderResourceView* m_srv = nullptr;   // not owned (the caller's, as SetInputView)
        int                       m_eyeW = 0, m_h = 0;
        DisplayOutput             m_output = DisplayOutput::Anaglyph;
        int                       m_anaColours = 0, m_anaStyle = 0;
        bool                      m_swap = false;
        bool                      m_dirty = true;
        float                     m_cbData[36] = {};
    };
}
