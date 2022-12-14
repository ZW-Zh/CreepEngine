#include "MSAAHelper.h"

using namespace DirectX;
using Microsoft::WRL::ComPtr;

MSAAHelper::MSAAHelper(DXGI_FORMAT backBufferFormat,
    DXGI_FORMAT depthBufferFormat,
    unsigned int sampleCount) :
        m_clearColor{},
        m_rtvDescriptorHeap{},
        m_dsvDescriptorHeap{},
        m_backBufferFormat(backBufferFormat),
        m_depthBufferFormat(depthBufferFormat),
        m_sampleCount(0),
        m_targetSampleCount(sampleCount),
        m_width(0),
        m_height(0)
{
    if (sampleCount < 2 || sampleCount > D3D12_MAX_MULTISAMPLE_SAMPLE_COUNT)
    {
        throw std::out_of_range("MSAA sample count invalid.");
    }
}


void MSAAHelper::SetDevice(ID3D12Device* device)
{
    if (device == m_device.Get())
        return;

    if (m_device)
    {
        ReleaseDevice();
    }

    {
        D3D12_FEATURE_DATA_FORMAT_SUPPORT formatSupport = { m_backBufferFormat, D3D12_FORMAT_SUPPORT1_NONE, D3D12_FORMAT_SUPPORT2_NONE };
        if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &formatSupport, sizeof(formatSupport))))
        {
            throw std::exception();
        }

        UINT required = D3D12_FORMAT_SUPPORT1_RENDER_TARGET | D3D12_FORMAT_SUPPORT1_MULTISAMPLE_RESOLVE | D3D12_FORMAT_SUPPORT1_MULTISAMPLE_RENDERTARGET;
        if ((formatSupport.Support1 & required) != required)
        {
            throw std::exception();
        }
    }

    {
        D3D12_FEATURE_DATA_FORMAT_SUPPORT formatSupport = { m_depthBufferFormat, D3D12_FORMAT_SUPPORT1_NONE, D3D12_FORMAT_SUPPORT2_NONE };
        if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &formatSupport, sizeof(formatSupport))))
        {
            throw std::exception();
        }

        UINT required = D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL | D3D12_FORMAT_SUPPORT1_MULTISAMPLE_RENDERTARGET;
        if ((formatSupport.Support1 & required) != required)
        {
            throw std::exception();
        }
    }

    for (m_sampleCount = m_targetSampleCount; m_sampleCount > 1; m_sampleCount--)
    {
        D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS levels = { m_backBufferFormat, m_sampleCount, D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE, 0u };
        if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &levels, sizeof(levels))))
            continue;

        if (levels.NumQualityLevels > 0)
            break;
    }

    if (m_sampleCount < 2)
    {
        throw std::exception();
    }

    // Create descriptor heaps for render target views and depth stencil views.
    D3D12_DESCRIPTOR_HEAP_DESC rtvDescriptorHeapDesc = {};
    rtvDescriptorHeapDesc.NumDescriptors = 1;
    rtvDescriptorHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;

    ThrowIfFailed(device->CreateDescriptorHeap(&rtvDescriptorHeapDesc,
        IID_PPV_ARGS(m_rtvDescriptorHeap.ReleaseAndGetAddressOf())));

    if (m_depthBufferFormat != DXGI_FORMAT_UNKNOWN)
    {
        D3D12_DESCRIPTOR_HEAP_DESC dsvDescriptorHeapDesc = {};
        dsvDescriptorHeapDesc.NumDescriptors = 1;
        dsvDescriptorHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;

        ThrowIfFailed(device->CreateDescriptorHeap(&dsvDescriptorHeapDesc,
            IID_PPV_ARGS(m_dsvDescriptorHeap.ReleaseAndGetAddressOf())));
    }

    m_device = device;
}


void MSAAHelper::SizeResources(size_t width, size_t height)
{
    if (width == m_width && height == m_height)
        return;

    if (m_width > UINT32_MAX || m_height > UINT32_MAX)
    {
        throw std::out_of_range("Invalid width/height");
    }

    if (!m_device)
        return;

    m_width = m_height = 0;

    CD3DX12_HEAP_PROPERTIES heapProperties(D3D12_HEAP_TYPE_DEFAULT);

    DXGI_FORMAT msaaFormat = m_backBufferFormat;

    // Create an MSAA render target
    D3D12_RESOURCE_DESC msaaRTDesc = CD3DX12_RESOURCE_DESC::Tex2D(
        msaaFormat,
        static_cast<UINT64>(width),
        static_cast<UINT>(height),
        1, // This render target view has only one texture.
        1, // Use a single mipmap level
        m_sampleCount
    );
    msaaRTDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE msaaOptimizedClearValue = {};
    msaaOptimizedClearValue.Format = m_backBufferFormat;
    memcpy(msaaOptimizedClearValue.Color, m_clearColor, sizeof(float) * 4);

    ThrowIfFailed(m_device->CreateCommittedResource(
        &heapProperties,
        D3D12_HEAP_FLAG_NONE,
        &msaaRTDesc,
        D3D12_RESOURCE_STATE_RESOLVE_SOURCE,
        &msaaOptimizedClearValue,
        IID_PPV_ARGS(m_msaaRenderTarget.ReleaseAndGetAddressOf())
    ));

    D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
    rtvDesc.Format = m_backBufferFormat;
    rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMS;

    m_device->CreateRenderTargetView(
        m_msaaRenderTarget.Get(), &rtvDesc,
        m_rtvDescriptorHeap->GetCPUDescriptorHandleForHeapStart());

    if (m_depthBufferFormat != DXGI_FORMAT_UNKNOWN)
    {
        // Create an MSAA depth stencil view
        D3D12_RESOURCE_DESC depthStencilDesc = CD3DX12_RESOURCE_DESC::Tex2D(
            m_depthBufferFormat,
            static_cast<UINT64>(width),
            static_cast<UINT>(height),
            1, // This depth stencil view has only one texture.
            1, // Use a single mipmap level.
            m_sampleCount
        );
        depthStencilDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        D3D12_CLEAR_VALUE depthOptimizedClearValue = {};
        depthOptimizedClearValue.Format = m_depthBufferFormat;
        depthOptimizedClearValue.DepthStencil.Depth = 1.0f;
        depthOptimizedClearValue.DepthStencil.Stencil = 0;

        ThrowIfFailed(m_device->CreateCommittedResource(
            &heapProperties,
            D3D12_HEAP_FLAG_NONE,
            &depthStencilDesc,
            D3D12_RESOURCE_STATE_DEPTH_WRITE,
            &depthOptimizedClearValue,
            IID_PPV_ARGS(m_msaaDepthStencil.ReleaseAndGetAddressOf())
        ));

        D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
        dsvDesc.Format = m_depthBufferFormat;
        dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMS;

        m_device->CreateDepthStencilView(
            m_msaaDepthStencil.Get(), &dsvDesc,
            m_dsvDescriptorHeap->GetCPUDescriptorHandleForHeapStart());
    }

    m_width = width;
    m_height = height;
}


void MSAAHelper::ReleaseDevice()
{
    m_rtvDescriptorHeap.Reset();
    m_dsvDescriptorHeap.Reset();

    m_msaaRenderTarget.Reset();
    m_msaaDepthStencil.Reset();

    m_device.Reset();

    m_width = m_height = 0;
}


void MSAAHelper::Prepare(ID3D12GraphicsCommandList* commandList, D3D12_RESOURCE_STATES beforeState)
{
    D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        m_msaaRenderTarget.Get(),
        beforeState,
        D3D12_RESOURCE_STATE_RENDER_TARGET);
    commandList->ResourceBarrier(1, &barrier);
}


void MSAAHelper::Resolve(ID3D12GraphicsCommandList* commandList, ID3D12Resource* backBuffer,
    D3D12_RESOURCE_STATES beforeState, D3D12_RESOURCE_STATES afterState)
{
    D3D12_RESOURCE_BARRIER barriers[2] =
    {
        CD3DX12_RESOURCE_BARRIER::Transition(m_msaaRenderTarget.Get(),
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_RESOLVE_SOURCE),
        CD3DX12_RESOURCE_BARRIER::Transition(backBuffer,
            beforeState,
            D3D12_RESOURCE_STATE_RESOLVE_DEST)
    };

    commandList->ResourceBarrier((beforeState != D3D12_RESOURCE_STATE_RESOLVE_DEST) ? 2u : 1u, barriers);

    commandList->ResolveSubresource(backBuffer, 0, m_msaaRenderTarget.Get(), 0, m_backBufferFormat);

    if (afterState != D3D12_RESOURCE_STATE_RESOLVE_DEST)
    {
        D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
            backBuffer,
            D3D12_RESOURCE_STATE_RESOLVE_DEST,
            afterState);
        commandList->ResourceBarrier(1, &barrier);
    }
}


void MSAAHelper::SetWindow(const RECT& output)
{
    // Determine the render target size in pixels.
    auto width = size_t(std::max<LONG>(output.right - output.left, 1));
    auto height = size_t(std::max<LONG>(output.bottom - output.top, 1));

    SizeResources(width, height);
}