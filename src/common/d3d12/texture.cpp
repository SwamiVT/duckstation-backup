#include "texture.h"
#include "../align.h"
#include "../assert.h"
#include "../log.h"
#include "context.h"
#include "staging_texture.h"
#include "stream_buffer.h"
#include "util.h"
Log_SetChannel(D3D12);

static constexpr std::array<DXGI_FORMAT, static_cast<u32>(GPUTexture::Format::Count)> s_dxgi_mapping = {
  {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B5G6R5_UNORM,
   DXGI_FORMAT_B5G5R5A1_UNORM, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_D16_UNORM}};

D3D12::Texture::Texture() = default;

D3D12::Texture::Texture(ID3D12Resource* resource, D3D12_RESOURCE_STATES state) : m_resource(std::move(resource))
{
  const D3D12_RESOURCE_DESC desc = GetDesc();
  m_width = static_cast<u16>(desc.Width);
  m_height = static_cast<u16>(desc.Height);
  m_layers = static_cast<u8>(desc.DepthOrArraySize);
  m_levels = static_cast<u8>(desc.MipLevels);
  m_samples = static_cast<u8>(desc.SampleDesc.Count);
  m_format = LookupBaseFormat(desc.Format);
}

D3D12::Texture::Texture(Texture&& texture)
  : m_resource(std::move(texture.m_resource)), m_srv_descriptor(texture.m_srv_descriptor),
    m_rtv_or_dsv_descriptor(texture.m_rtv_or_dsv_descriptor), m_is_depth_view(texture.m_is_depth_view)
{
  m_width = texture.m_width;
  m_height = texture.m_height;
  m_layers = texture.m_layers;
  m_levels = texture.m_levels;
  m_samples = texture.m_samples;
  texture.m_srv_descriptor = {};
  texture.m_rtv_or_dsv_descriptor = {};
  texture.m_state = D3D12_RESOURCE_STATE_COMMON;
  texture.m_is_depth_view = false;
  texture.ClearBaseProperties();
}

DXGI_FORMAT D3D12::Texture::GetDXGIFormat(Format format)
{
  return s_dxgi_mapping[static_cast<u8>(format)];
}

GPUTexture::Format D3D12::Texture::LookupBaseFormat(DXGI_FORMAT dformat)
{
  for (u32 i = 0; i < static_cast<u32>(s_dxgi_mapping.size()); i++)
  {
    if (s_dxgi_mapping[i] == dformat)
      return static_cast<Format>(i);
  }
  return GPUTexture::Format::Unknown;
}

D3D12::Texture::~Texture()
{
  Destroy();
}

D3D12::Texture& D3D12::Texture::operator=(Texture&& texture)
{
  Destroy();

  m_width = texture.m_width;
  m_height = texture.m_height;
  m_layers = texture.m_layers;
  m_levels = texture.m_levels;
  m_samples = texture.m_samples;

  m_resource = std::move(texture.m_resource);
  m_srv_descriptor = texture.m_srv_descriptor;
  m_rtv_or_dsv_descriptor = texture.m_rtv_or_dsv_descriptor;
  m_state = texture.m_state;
  m_is_depth_view = texture.m_is_depth_view;

  texture.ClearBaseProperties();
  texture.m_srv_descriptor = {};
  texture.m_rtv_or_dsv_descriptor = {};
  texture.m_state = D3D12_RESOURCE_STATE_COMMON;
  texture.m_is_depth_view = false;
  return *this;
}

D3D12_RESOURCE_DESC D3D12::Texture::GetDesc() const
{
  return m_resource->GetDesc();
}

bool D3D12::Texture::IsValid() const
{
  return static_cast<bool>(m_resource);
}

bool D3D12::Texture::Create(u32 width, u32 height, u32 layers, u32 levels, u32 samples, DXGI_FORMAT format,
                            DXGI_FORMAT srv_format, DXGI_FORMAT rtv_format, DXGI_FORMAT dsv_format,
                            D3D12_RESOURCE_FLAGS flags)
{
  constexpr D3D12_HEAP_PROPERTIES heap_properties = {D3D12_HEAP_TYPE_DEFAULT};

  if (width > MAX_WIDTH || height > MAX_HEIGHT || layers > MAX_LAYERS || levels > MAX_LEVELS || samples > MAX_SAMPLES)
  {
    Log_ErrorPrintf("Invalid dimensions: %ux%ux%u %u %u", width, height, layers, levels, samples);
    return false;
  }

  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = width;
  desc.Height = static_cast<u16>(height);
  desc.DepthOrArraySize = static_cast<u16>(layers);
  desc.MipLevels = static_cast<u16>(levels);
  desc.Format = format;
  desc.SampleDesc.Count = samples;
  desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  desc.Flags = flags;

  D3D12_CLEAR_VALUE optimized_clear_value = {};
  D3D12_RESOURCE_STATES state;
  if (rtv_format != DXGI_FORMAT_UNKNOWN)
  {
    optimized_clear_value.Format = rtv_format;
    state = D3D12_RESOURCE_STATE_RENDER_TARGET;
  }
  else if (dsv_format != DXGI_FORMAT_UNKNOWN)
  {
    optimized_clear_value.Format = dsv_format;
    state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
  }
  else
  {
    state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  }

  ComPtr<ID3D12Resource> resource;
  HRESULT hr = g_d3d12_context->GetDevice()->CreateCommittedResource(
    &heap_properties, D3D12_HEAP_FLAG_NONE, &desc, state,
    (rtv_format != DXGI_FORMAT_UNKNOWN || dsv_format != DXGI_FORMAT_UNKNOWN) ? &optimized_clear_value : nullptr,
    IID_PPV_ARGS(resource.GetAddressOf()));
  if (FAILED(hr))
  {
    Log_ErrorPrintf("Create texture failed: 0x%08X", hr);
    return false;
  }

  DescriptorHandle srv_descriptor, rtv_descriptor;
  bool is_depth_view = false;
  if (srv_format != DXGI_FORMAT_UNKNOWN)
  {
    if (!CreateSRVDescriptor(resource.Get(), srv_format, samples > 1, &srv_descriptor))
      return false;
  }

  if (rtv_format != DXGI_FORMAT_UNKNOWN)
  {
    Assert(dsv_format == DXGI_FORMAT_UNKNOWN);
    if (!CreateRTVDescriptor(resource.Get(), rtv_format, samples > 1, &rtv_descriptor))
    {
      g_d3d12_context->GetDescriptorHeapManager().Free(&srv_descriptor);
      return false;
    }
  }
  else if (dsv_format != DXGI_FORMAT_UNKNOWN)
  {
    if (!CreateDSVDescriptor(resource.Get(), dsv_format, samples > 1, &rtv_descriptor))
    {
      g_d3d12_context->GetDescriptorHeapManager().Free(&srv_descriptor);
      return false;
    }

    is_depth_view = true;
  }

  Destroy(true);

  m_resource = std::move(resource);
  m_srv_descriptor = std::move(srv_descriptor);
  m_rtv_or_dsv_descriptor = std::move(rtv_descriptor);
  m_width = static_cast<u16>(width);
  m_height = static_cast<u16>(height);
  m_layers = static_cast<u8>(layers);
  m_levels = static_cast<u8>(levels);
  m_samples = static_cast<u8>(samples);
  m_format = LookupBaseFormat(format);
  m_state = state;
  m_is_depth_view = is_depth_view;
  return true;
}

bool D3D12::Texture::Adopt(ComPtr<ID3D12Resource> texture, DXGI_FORMAT srv_format, DXGI_FORMAT rtv_format,
                           DXGI_FORMAT dsv_format, D3D12_RESOURCE_STATES state)
{
  const D3D12_RESOURCE_DESC desc(texture->GetDesc());

  DescriptorHandle srv_descriptor, rtv_descriptor;
  if (srv_format != DXGI_FORMAT_UNKNOWN)
  {
    if (!CreateSRVDescriptor(texture.Get(), srv_format, desc.SampleDesc.Count > 1, &srv_descriptor))
      return false;
  }

  m_is_depth_view = false;

  if (rtv_format != DXGI_FORMAT_UNKNOWN)
  {
    Assert(dsv_format == DXGI_FORMAT_UNKNOWN);
    if (!CreateRTVDescriptor(texture.Get(), rtv_format, desc.SampleDesc.Count > 1, &rtv_descriptor))
    {
      g_d3d12_context->GetDescriptorHeapManager().Free(&srv_descriptor);
      return false;
    }
  }
  else if (dsv_format != DXGI_FORMAT_UNKNOWN)
  {
    if (!CreateDSVDescriptor(texture.Get(), dsv_format, desc.SampleDesc.Count > 1, &rtv_descriptor))
    {
      g_d3d12_context->GetDescriptorHeapManager().Free(&srv_descriptor);
      return false;
    }

    m_is_depth_view = true;
  }

  m_resource = std::move(texture);
  m_srv_descriptor = std::move(srv_descriptor);
  m_rtv_or_dsv_descriptor = std::move(rtv_descriptor);
  m_width = static_cast<u16>(desc.Width);
  m_height = static_cast<u16>(desc.Height);
  m_layers = static_cast<u8>(desc.DepthOrArraySize);
  m_levels = static_cast<u8>(desc.MipLevels);
  m_samples = static_cast<u8>(desc.SampleDesc.Count);
  m_format = LookupBaseFormat(desc.Format);
  m_state = state;
  return true;
}

void D3D12::Texture::Destroy(bool defer /* = true */)
{
  if (defer)
  {
    g_d3d12_context->DeferDescriptorDestruction(g_d3d12_context->GetDescriptorHeapManager(), &m_srv_descriptor);
    if (m_is_depth_view)
      g_d3d12_context->DeferDescriptorDestruction(g_d3d12_context->GetDSVHeapManager(), &m_rtv_or_dsv_descriptor);
    else
      g_d3d12_context->DeferDescriptorDestruction(g_d3d12_context->GetRTVHeapManager(), &m_rtv_or_dsv_descriptor);
    g_d3d12_context->DeferResourceDestruction(m_resource.Get());
    m_resource.Reset();
  }
  else
  {
    g_d3d12_context->GetDescriptorHeapManager().Free(&m_srv_descriptor);
    if (m_is_depth_view)
      g_d3d12_context->GetDSVHeapManager().Free(&m_rtv_or_dsv_descriptor);
    else
      g_d3d12_context->GetRTVHeapManager().Free(&m_rtv_or_dsv_descriptor);

    m_resource.Reset();
  }

  ClearBaseProperties();
  m_is_depth_view = false;
}

void D3D12::Texture::TransitionToState(D3D12_RESOURCE_STATES state) const
{
  if (m_state == state)
    return;

  ResourceBarrier(g_d3d12_context->GetCommandList(), m_resource.Get(), m_state, state);
  m_state = state;
}

bool D3D12::Texture::BeginStreamUpdate(u32 x, u32 y, u32 width, u32 height, void** out_data, u32* out_data_pitch)
{
  const u32 copy_pitch = Common::AlignUpPow2(width * GetPixelSize(), D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
  const u32 upload_size = copy_pitch * height;

  if (!g_d3d12_context->GetTextureStreamBuffer().ReserveMemory(upload_size, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT))
  {
    Log_PerfPrintf("Executing command buffer while waiting for %u bytes (%ux%u) in upload buffer", upload_size, width,
                   height);
    g_d3d12_context->ExecuteCommandList(false);
    if (!g_d3d12_context->GetTextureStreamBuffer().ReserveMemory(upload_size, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT))
    {
      Log_ErrorPrintf("Failed to reserve %u bytes for %ux%u upload", upload_size, width, height);
      return false;
    }
  }

  *out_data = g_d3d12_context->GetTextureStreamBuffer().GetCurrentHostPointer();
  *out_data_pitch = copy_pitch;
  return true;
}

void D3D12::Texture::EndStreamUpdate(u32 x, u32 y, u32 width, u32 height)
{
  const u32 copy_pitch = Common::AlignUpPow2(width * GetPixelSize(), D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
  const u32 upload_size = copy_pitch * height;

  StreamBuffer& sb = g_d3d12_context->GetTextureStreamBuffer();
  const u32 sb_offset = sb.GetCurrentOffset();
  sb.CommitMemory(upload_size);

  CopyFromBuffer(x, y, width, height, copy_pitch, sb.GetBuffer(), sb_offset);
}

void D3D12::Texture::CopyFromBuffer(u32 x, u32 y, u32 width, u32 height, u32 pitch, ID3D12Resource* buffer,
                                    u32 buffer_offset)
{
  D3D12_TEXTURE_COPY_LOCATION src;
  src.pResource = buffer;
  src.SubresourceIndex = 0;
  src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  src.PlacedFootprint.Offset = buffer_offset;
  src.PlacedFootprint.Footprint.Width = width;
  src.PlacedFootprint.Footprint.Height = height;
  src.PlacedFootprint.Footprint.Depth = 1;
  src.PlacedFootprint.Footprint.RowPitch = pitch;
  src.PlacedFootprint.Footprint.Format = GetDXGIFormat();

  D3D12_TEXTURE_COPY_LOCATION dst;
  dst.pResource = m_resource.Get();
  dst.SubresourceIndex = 0;
  dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;

  const D3D12_BOX src_box{0u, 0u, 0u, width, height, 1u};
  const D3D12_RESOURCE_STATES old_state = m_state;
  TransitionToState(D3D12_RESOURCE_STATE_COPY_DEST);
  g_d3d12_context->GetCommandList()->CopyTextureRegion(&dst, x, y, 0, &src, &src_box);
  TransitionToState(old_state);
}

bool D3D12::Texture::LoadData(u32 x, u32 y, u32 width, u32 height, const void* data, u32 pitch)
{
  const u32 texel_size = GetPixelSize();
  const u32 upload_pitch = Common::AlignUpPow2(width * texel_size, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
  const u32 upload_size = upload_pitch * height;
  if (upload_size >= g_d3d12_context->GetTextureStreamBuffer().GetSize())
  {
    StagingTexture st;
    if (!st.Create(width, height, GetDXGIFormat(), true) || !st.WritePixels(0, 0, width, height, data, pitch))
      return false;

    D3D12_RESOURCE_STATES old_state = m_state;
    TransitionToState(D3D12_RESOURCE_STATE_COPY_DEST);
    st.CopyToTexture(0, 0, m_resource.Get(), 0, x, y, width, height);
    st.Destroy(true);
    TransitionToState(old_state);
    return true;
  }

  void* write_ptr;
  u32 write_pitch;
  if (!BeginStreamUpdate(x, y, width, height, &write_ptr, &write_pitch))
    return false;

  CopyToUploadBuffer(data, pitch, height, write_ptr, write_pitch);
  EndStreamUpdate(x, y, width, height);
  return true;
}

void D3D12::Texture::CopyToUploadBuffer(const void* src_data, u32 src_pitch, u32 height, void* dst_data, u32 dst_pitch)
{
  const u8* src_ptr = static_cast<const u8*>(src_data);
  u8* dst_ptr = static_cast<u8*>(dst_data);
  if (src_pitch == dst_pitch)
  {
    std::memcpy(dst_ptr, src_ptr, dst_pitch * height);
  }
  else
  {
    const u32 copy_size = std::min(src_pitch, dst_pitch);
    for (u32 row = 0; row < height; row++)
    {
      std::memcpy(dst_ptr, src_ptr, copy_size);
      src_ptr += src_pitch;
      dst_ptr += dst_pitch;
    }
  }
}

bool D3D12::Texture::CreateSRVDescriptor(ID3D12Resource* resource, DXGI_FORMAT format, bool multisampled,
                                         DescriptorHandle* dh)
{
  if (!g_d3d12_context->GetDescriptorHeapManager().Allocate(dh))
  {
    Log_ErrorPrintf("Failed to allocate SRV descriptor");
    return false;
  }

  D3D12_SHADER_RESOURCE_VIEW_DESC desc = {
    format, multisampled ? D3D12_SRV_DIMENSION_TEXTURE2DMS : D3D12_SRV_DIMENSION_TEXTURE2D,
    D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING};
  if (!multisampled)
    desc.Texture2D.MipLevels = 1;

  g_d3d12_context->GetDevice()->CreateShaderResourceView(resource, &desc, dh->cpu_handle);
  return true;
}

bool D3D12::Texture::CreateRTVDescriptor(ID3D12Resource* resource, DXGI_FORMAT format, bool multisampled,
                                         DescriptorHandle* dh)
{
  if (!g_d3d12_context->GetRTVHeapManager().Allocate(dh))
  {
    Log_ErrorPrintf("Failed to allocate SRV descriptor");
    return false;
  }

  D3D12_RENDER_TARGET_VIEW_DESC desc = {format,
                                        multisampled ? D3D12_RTV_DIMENSION_TEXTURE2DMS : D3D12_RTV_DIMENSION_TEXTURE2D};

  g_d3d12_context->GetDevice()->CreateRenderTargetView(resource, &desc, dh->cpu_handle);
  return true;
}

bool D3D12::Texture::CreateDSVDescriptor(ID3D12Resource* resource, DXGI_FORMAT format, bool multisampled,
                                         DescriptorHandle* dh)
{
  if (!g_d3d12_context->GetDSVHeapManager().Allocate(dh))
  {
    Log_ErrorPrintf("Failed to allocate SRV descriptor");
    return false;
  }

  D3D12_DEPTH_STENCIL_VIEW_DESC desc = {
    format, multisampled ? D3D12_DSV_DIMENSION_TEXTURE2DMS : D3D12_DSV_DIMENSION_TEXTURE2D, D3D12_DSV_FLAG_NONE};

  g_d3d12_context->GetDevice()->CreateDepthStencilView(resource, &desc, dh->cpu_handle);
  return true;
}
