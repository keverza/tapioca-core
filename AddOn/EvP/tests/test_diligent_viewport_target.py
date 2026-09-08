from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
ARCHVIZ = ROOT / "Sources" / "AddOn" / "ArchViz"


def _sources() -> tuple[str, str]:
    header = (ARCHVIZ / "DiligentViewportTarget.hpp").read_text(encoding="utf-8")
    source = (ARCHVIZ / "DiligentViewportTarget.cpp").read_text(encoding="utf-8")
    return header, source


def test_viewport_target_owns_sampleable_depth_for_every_mode() -> None:
    _, source = _sources()

    assert "desc.DepthBufferFormat = Diligent::TEX_FORMAT_UNKNOWN" in source
    assert "Diligent::BIND_DEPTH_STENCIL | Diligent::BIND_SHADER_RESOURCE" in source
    assert "GetDepthBufferDSV" not in source
    assert source.count("impl_->depthTexture->GetDefaultView (Diligent::TEXTURE_VIEW_DEPTH_STENCIL)") == 3


def test_depth_creation_uses_explicit_dimensions_and_resize_retries_failure() -> None:
    _, source = _sources()

    assert "CreateDepth (uint32_t requestedWidth, uint32_t requestedHeight" in source
    assert "dd.Width = requestedWidth" in source
    assert "dd.Height = requestedHeight" in source
    assert "CreateDepth (w, h, resizedDepth, depthError)" in source
    assert source.index("CreateDepth (w, h, resizedDepth, depthError)") < source.index(
        "ResizeBuffers (0, w, h"
    )
    assert "impl_->depthTexture = std::move (resizedDepth)" in source
    assert "GetCurrentBackBufferRTV" in source
    assert "resizedDesc->Width == w && resizedDesc->Height == h" in source
    assert "CreatePaletteSwapChain (w, h, recreateError)" in source
    assert 'depthError + " -- retrying on the next frame"' in source
    assert "impl_->resizePending = true" in source
    assert "impl_->mode == SurfaceMode::Offscreen" in source


def test_depth_shader_view_exposes_the_owned_default_srv() -> None:
    header, source = _sources()

    assert "Diligent::ITextureView* DepthShaderView () const;" in header
    assert "DiligentViewportTarget::DepthShaderView () const" in source
    assert "GetDefaultView (Diligent::TEXTURE_VIEW_SHADER_RESOURCE)" in source
