// S0 spike 2: a captured window as an OpenXR quad layer in the headset.
//   s2_quad <title or exe> [secs=30] [--distance m=1.2] [--width m=1.2]
// The compositor samples the panel itself at the lens (no reprojection of our picture), which is what keeps text
// sharp and costs us next to nothing. The swapchain is only written when the window sends a new frame.
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include "common/capture.h"
#include <d3dcompiler.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace fi;

static XrInstance g_instance = XR_NULL_HANDLE;

#define XR(call)                                                                     \
  do {                                                                               \
    XrResult r_ = (call);                                                            \
    if (XR_FAILED(r_)) {                                                             \
      char s_[XR_MAX_RESULT_STRING_SIZE] = "?";                                      \
      if (g_instance) xrResultToString(g_instance, r_, s_);                          \
      fprintf(stderr, "%s failed: %s (%d)\n", #call, s_, (int)r_);                   \
      return 1;                                                                      \
    }                                                                                \
  } while (0)

static const char* kShader = R"(
struct VO { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VO vs(uint id : SV_VertexID) {
  VO o; o.uv = float2((id << 1) & 2, id & 2);
  o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1); return o;
}
Texture2D tex : register(t0); SamplerState smp : register(s0);
float4 ps(VO i) : SV_Target { return float4(tex.Sample(smp, i.uv).rgb, 1); }
)";

static bool is_srgb(int64_t f) {
  return f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
}

static int run(int argc, wchar_t** argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: s2_quad <title or exe> [secs] [--distance m] [--width m]\n");
    return 2;
  }
  double secs = 30, distance = 1.2, width_m = 1.2;
  for (int i = 2; i < argc; ++i) {
    std::wstring_view a = argv[i];
    if (a == L"--distance" && i + 1 < argc) distance = _wtof(argv[++i]);
    else if (a == L"--width" && i + 1 < argc) width_m = _wtof(argv[++i]);
    else secs = _wtof(argv[i]);
  }
  HWND hwnd = find_window(argv[1]);
  if (!hwnd) {
    fprintf(stderr, "No window matches \"%s\".\n", narrow(argv[1]).c_str());
    return 1;
  }

  // The instance, the headset, and the GPU the runtime wants us on.
  const char* exts[] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
  XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
  strcpy_s(ici.applicationInfo.applicationName, "FullImmersion S2");
  strcpy_s(ici.applicationInfo.engineName, "FullImmersion");
  ici.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
  ici.enabledExtensionCount = 1;
  ici.enabledExtensionNames = exts;
  XR(xrCreateInstance(&ici, &g_instance));
  XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
  XR(xrGetInstanceProperties(g_instance, &ip));
  printf("runtime           %s %u.%u.%u\n", ip.runtimeName, XR_VERSION_MAJOR(ip.runtimeVersion),
         XR_VERSION_MINOR(ip.runtimeVersion), XR_VERSION_PATCH(ip.runtimeVersion));

  XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
  sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
  XrSystemId system = XR_NULL_SYSTEM_ID;
  XR(xrGetSystem(g_instance, &sgi, &system));
  XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
  XR(xrGetSystemProperties(g_instance, system, &sp));
  printf("headset           %s (max %u layers)\n", sp.systemName, sp.graphicsProperties.maxLayerCount);

  PFN_xrGetD3D11GraphicsRequirementsKHR get_req = nullptr;
  XR(xrGetInstanceProcAddr(g_instance, "xrGetD3D11GraphicsRequirementsKHR",
                           reinterpret_cast<PFN_xrVoidFunction*>(&get_req)));
  XrGraphicsRequirementsD3D11KHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
  XR(get_req(g_instance, system, &req));

  winrt::com_ptr<IDXGIFactory1> factory;
  winrt::check_hresult(CreateDXGIFactory1(__uuidof(IDXGIFactory1), factory.put_void()));
  winrt::com_ptr<IDXGIAdapter1> adapter;
  for (UINT i = 0;; ++i) {
    winrt::com_ptr<IDXGIAdapter1> a;
    if (factory->EnumAdapters1(i, a.put()) == DXGI_ERROR_NOT_FOUND) break;
    DXGI_ADAPTER_DESC1 d{};
    a->GetDesc1(&d);
    if (memcmp(&d.AdapterLuid, &req.adapterLuid, sizeof(LUID)) == 0) {
      adapter = a;
      printf("gpu               %s\n", narrow(d.Description).c_str());
      break;
    }
  }
  auto device = make_device(adapter.get());
  winrt::com_ptr<ID3D11DeviceContext> ctx;
  device->GetImmediateContext(ctx.put());

  // The window.
  WindowCapture cap(device.get(), hwnd);
  if (!cap.wait_frame(2000)) {
    fprintf(stderr, "No frame from the window.\n");
    return 1;
  }
  printf("window            %s, %u x %u\n", narrow(describe(hwnd).title).c_str(), cap.width(), cap.height());

  // The session and the room.
  XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
  binding.device = device.get();
  XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
  sci.next = &binding;
  sci.systemId = system;
  XrSession session = XR_NULL_HANDLE;
  XR(xrCreateSession(g_instance, &sci, &session));
  XrReferenceSpaceCreateInfo rsi{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
  rsi.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
  rsi.poseInReferenceSpace.orientation.w = 1;
  XrSpace space = XR_NULL_HANDLE;
  XR(xrCreateReferenceSpace(session, &rsi, &space));

  // The panel's swapchain: the window's size (to 4K), in the best format the runtime offers.
  uint32_t nf = 0;
  XR(xrEnumerateSwapchainFormats(session, 0, &nf, nullptr));
  std::vector<int64_t> formats(nf);
  XR(xrEnumerateSwapchainFormats(session, nf, &nf, formats.data()));
  int64_t prefer[] = {DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM,
                      DXGI_FORMAT_R8G8B8A8_UNORM};
  int64_t format = 0;
  for (auto p : prefer)
    if (std::find(formats.begin(), formats.end(), p) != formats.end()) {
      format = p;
      break;
    }
  if (!format) {
    fprintf(stderr, "The runtime offers no 8-bit colour swapchain.\n");
    return 1;
  }
  uint32_t sw = std::min<uint32_t>(cap.width(), 3840), sh = std::min<uint32_t>(cap.height(), 3840);
  XrSwapchainCreateInfo sci2{XR_TYPE_SWAPCHAIN_CREATE_INFO};
  sci2.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
  sci2.format = format;
  sci2.sampleCount = 1;
  sci2.width = sw;
  sci2.height = sh;
  sci2.faceCount = 1;
  sci2.arraySize = 1;
  sci2.mipCount = 1;
  XrSwapchain swapchain = XR_NULL_HANDLE;
  XR(xrCreateSwapchain(session, &sci2, &swapchain));
  uint32_t ni = 0;
  XR(xrEnumerateSwapchainImages(swapchain, 0, &ni, nullptr));
  std::vector<XrSwapchainImageD3D11KHR> images(ni, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
  XR(xrEnumerateSwapchainImages(swapchain, ni, &ni, reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())));
  std::vector<winrt::com_ptr<ID3D11RenderTargetView>> rtvs(ni);
  for (uint32_t i = 0; i < ni; ++i) {
    D3D11_RENDER_TARGET_VIEW_DESC rd{};
    rd.Format = (DXGI_FORMAT)format;
    rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    winrt::check_hresult(device->CreateRenderTargetView(images[i].texture, &rd, rtvs[i].put()));
  }
  bool srgb = is_srgb(format);
  printf("swapchain         %u x %u, DXGI format %lld%s, %u images\n", sw, sh, format, srgb ? " (sRGB)" : "", ni);

  // The blit: capture -> swapchain image, stretched if the window has since been resized.
  winrt::com_ptr<ID3DBlob> vsb, psb, err;
  if (FAILED(D3DCompile(kShader, strlen(kShader), "blit", nullptr, nullptr, "vs", "vs_5_0", 0, 0, vsb.put(),
                        err.put())) ||
      FAILED(D3DCompile(kShader, strlen(kShader), "blit", nullptr, nullptr, "ps", "ps_5_0", 0, 0, psb.put(),
                        err.put()))) {
    fprintf(stderr, "Shader: %s\n", err ? (const char*)err->GetBufferPointer() : "?");
    return 1;
  }
  winrt::com_ptr<ID3D11VertexShader> vs;
  winrt::com_ptr<ID3D11PixelShader> ps;
  winrt::check_hresult(device->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, vs.put()));
  winrt::check_hresult(device->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, ps.put()));
  D3D11_SAMPLER_DESC smd{};
  smd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  smd.AddressU = smd.AddressV = smd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  smd.MaxLOD = D3D11_FLOAT32_MAX;
  winrt::com_ptr<ID3D11SamplerState> sampler;
  winrt::check_hresult(device->CreateSamplerState(&smd, sampler.put()));

  auto blit = [&](uint32_t index) {
    D3D11_VIEWPORT vp{0, 0, float(sw), float(sh), 0, 1};
    ID3D11RenderTargetView* rtv = rtvs[index].get();
    ID3D11ShaderResourceView* srv = cap.srv(srgb);
    ID3D11SamplerState* smp = sampler.get();
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ctx->RSSetViewports(1, &vp);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs.get(), nullptr, 0);
    ctx->PSSetShader(ps.get(), nullptr, 0);
    ctx->PSSetShaderResources(0, 1, &srv);
    ctx->PSSetSamplers(0, 1, &smp);
    ctx->Draw(3, 0);
    ID3D11ShaderResourceView* none = nullptr;
    ctx->PSSetShaderResources(0, 1, &none);
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
  };

  // The loop.
  bool running = true, session_running = false, have_image = true, exit_requested = false;
  bool first = true;
  XrSessionState state = XR_SESSION_STATE_UNKNOWN;
  double t_begin = now_ms(), t_running = 0, last_wait = 0, sum_interval = 0, max_interval = 0, sum_cpu = 0;
  uint64_t xr_frames = 0, writes = 0, rendered = 0;
  int64_t period_ns = 0;
  have_image = false;

  while (running) {
    XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(g_instance, &ev) == XR_SUCCESS) {
      if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
        state = reinterpret_cast<XrEventDataSessionStateChanged*>(&ev)->state;
        printf("session           state %d\n", (int)state);
        if (state == XR_SESSION_STATE_READY) {
          XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
          bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
          XR(xrBeginSession(session, &bi));
          session_running = true;
          t_running = now_ms();
        } else if (state == XR_SESSION_STATE_STOPPING) {
          XR(xrEndSession(session));
          session_running = false;
        } else if (state == XR_SESSION_STATE_EXITING || state == XR_SESSION_STATE_LOSS_PENDING) {
          running = false;
        }
      } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
        running = false;
      }
      ev = {XR_TYPE_EVENT_DATA_BUFFER};
    }
    if (!running) break;
    if (!session_running) {
      if (now_ms() - t_begin > 20000 && t_running == 0) {
        fprintf(stderr, "The session never became ready in 20 s: is SteamVR running and the headset on?\n");
        break;
      }
      Sleep(10);
      continue;
    }
    if (!exit_requested && now_ms() - t_running > secs * 1000.0) {
      xrRequestExitSession(session);
      exit_requested = true;
    }

    XrFrameState fs{XR_TYPE_FRAME_STATE};
    XR(xrWaitFrame(session, nullptr, &fs));
    double t_frame = now_ms();
    if (last_wait > 0) {
      double dt = t_frame - last_wait;
      sum_interval += dt;
      max_interval = std::max(max_interval, dt);
    }
    last_wait = t_frame;
    period_ns = fs.predictedDisplayPeriod;
    XR(xrBeginFrame(session, nullptr));

    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    std::vector<XrCompositionLayerBaseHeader*> layers;
    if (fs.shouldRender) {
      ++rendered;
      bool fresh = cap.poll();
      if (fresh || first) {
        uint32_t index = 0;
        XR(xrAcquireSwapchainImage(swapchain, nullptr, &index));
        XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wi.timeout = XR_INFINITE_DURATION;
        XR(xrWaitSwapchainImage(swapchain, &wi));
        blit(index);
        XR(xrReleaseSwapchainImage(swapchain, nullptr));
        ++writes;
        first = false;
        have_image = true;
      }
      if (have_image) {
        quad.space = space;
        quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        quad.subImage.swapchain = swapchain;
        quad.subImage.imageRect = {{0, 0}, {(int32_t)sw, (int32_t)sh}};
        quad.pose.orientation.w = 1;
        quad.pose.position = {0, 0, -(float)distance};
        quad.size = {(float)width_m, (float)(width_m * double(sh) / double(sw))};
        layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&quad));
      }
    }
    sum_cpu += now_ms() - t_frame;

    XrFrameEndInfo fe{XR_TYPE_FRAME_END_INFO};
    fe.displayTime = fs.predictedDisplayTime;
    fe.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    fe.layerCount = (uint32_t)layers.size();
    fe.layers = layers.data();
    XR(xrEndFrame(session, &fe));
    ++xr_frames;
  }

  double ran = t_running > 0 ? (now_ms() - t_running) / 1000.0 : 0;
  auto& s = cap.stats();
  printf("ran               %.1f s, %llu frames (%llu shown)\n", ran, xr_frames, rendered);
  if (period_ns) printf("headset rate      %.1f Hz\n", 1e9 / double(period_ns));
  if (xr_frames > 1)
    printf("frame interval    avg %.2f ms, worst %.2f ms\n", sum_interval / double(xr_frames - 1), max_interval);
  if (xr_frames) printf("our cpu / frame   %.3f ms (poll + blit + submit)\n", sum_cpu / double(xr_frames));
  printf("panel writes      %llu (window frames %llu, capture latency avg %.2f ms)\n", writes, s.frames,
         s.avg_latency_ms());

  xrDestroySwapchain(swapchain);
  xrDestroySpace(space);
  xrDestroySession(session);
  xrDestroyInstance(g_instance);
  return 0;
}

int wmain(int argc, wchar_t** argv) {
  make_dpi_aware();
  winrt::init_apartment(winrt::apartment_type::multi_threaded);
  try {
    return run(argc, argv);
  } catch (winrt::hresult_error const& e) {
    fprintf(stderr, "Failed: 0x%08X %s\n", (unsigned)e.code().value, narrow(e.message()).c_str());
    return 1;
  }
}
