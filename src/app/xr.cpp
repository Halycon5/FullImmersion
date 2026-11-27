#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include "app/xr.h"
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace fi {

namespace {

Pose to_pose(const XrPosef& p) {
  return {{p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w},
          {p.position.x, p.position.y, p.position.z}};
}
XrPosef to_xr(const Pose& p) {
  XrPosef o;
  o.orientation = {p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w};
  o.position = {p.position.x, p.position.y, p.position.z};
  return o;
}

struct Chain {
  XrSwapchain handle = XR_NULL_HANDLE;
  uint32_t w = 0, h = 0;
  std::vector<winrt::com_ptr<ID3D11RenderTargetView>> rtvs;
  int unused_frames = 0;
};

}  // namespace

struct Xr::Impl {
  XrInstance instance = XR_NULL_HANDLE;
  XrSystemId system = XR_NULL_SYSTEM_ID;
  XrSession session = XR_NULL_HANDLE;
  XrSpace local = XR_NULL_HANDLE, stage = XR_NULL_HANDLE, view_space = XR_NULL_HANDLE;
  XrSessionState state = XR_SESSION_STATE_UNKNOWN;
  bool running = false, ended = false;
  LUID luid{};
  std::string runtime;
  uint32_t max_layers = 16;
  float floor = -1.2f;
  bool floor_known = false;

  ID3D11Device* device = nullptr;
  Renderer* renderer = nullptr;
  int64_t format = 0;
  Chain eyes[2];
  Chain dot;
  bool dot_drawn = false;
  std::map<uint64_t, Chain> panels;
  XrViewConfigurationView eye_cfg[2]{{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};

  XrActionSet set = XR_NULL_HANDLE;
  XrAction aim = XR_NULL_HANDLE, select = XR_NULL_HANDLE, squeeze = XR_NULL_HANDLE, alt = XR_NULL_HANDLE,
           stick = XR_NULL_HANDLE, haptic = XR_NULL_HANDLE;
  XrPath hand_path[2]{};
  XrSpace hand_space[2]{};
  XrTime last_time = 0;
  XrFrameState frame_state{XR_TYPE_FRAME_STATE};
  XrView eye_views_[2]{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};

  XrPath path(const char* s) {
    XrPath p = XR_NULL_PATH;
    xrStringToPath(instance, s, &p);
    return p;
  }

  bool make_chain(Chain& c, uint32_t w, uint32_t h, std::string& err) {
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    ci.format = format;
    ci.sampleCount = 1;
    ci.width = w;
    ci.height = h;
    ci.faceCount = 1;
    ci.arraySize = 1;
    ci.mipCount = 1;
    if (XR_FAILED(xrCreateSwapchain(session, &ci, &c.handle))) {
      err = "xrCreateSwapchain failed";
      return false;
    }
    c.w = w;
    c.h = h;
    uint32_t n = 0;
    xrEnumerateSwapchainImages(c.handle, 0, &n, nullptr);
    std::vector<XrSwapchainImageD3D11KHR> images(n, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    xrEnumerateSwapchainImages(c.handle, n, &n, reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));
    c.rtvs.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
      D3D11_RENDER_TARGET_VIEW_DESC rd{};
      rd.Format = (DXGI_FORMAT)format;
      rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
      if (FAILED(device->CreateRenderTargetView(images[i].texture, &rd, c.rtvs[i].put()))) {
        err = "CreateRenderTargetView on a swapchain image failed";
        return false;
      }
    }
    return true;
  }
  void drop_chain(Chain& c) {
    c.rtvs.clear();
    if (c.handle) xrDestroySwapchain(c.handle);
    c.handle = XR_NULL_HANDLE;
  }
  template <class F>
  bool write(Chain& c, F&& draw) {
    uint32_t index = 0;
    if (XR_FAILED(xrAcquireSwapchainImage(c.handle, nullptr, &index))) return false;
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    if (XR_FAILED(xrWaitSwapchainImage(c.handle, &wi))) return false;
    draw(c.rtvs[index].get());
    xrReleaseSwapchainImage(c.handle, nullptr);
    return true;
  }

  void find_floor() {
    if (!stage || floor_known) return;
    XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
    if (XR_SUCCEEDED(xrLocateSpace(local, stage, frame_state.predictedDisplayTime, &loc)) &&
        (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) && loc.pose.position.y > 0.3f) {
      floor = -loc.pose.position.y;  // the room's origin is where your head was; the floor is that far below
      floor_known = true;
    }
  }
};

Xr::Xr() : m(std::make_unique<Impl>()) {}

Xr::~Xr() {
  for (auto& [k, c] : m->panels) m->drop_chain(c);
  m->drop_chain(m->eyes[0]);
  m->drop_chain(m->eyes[1]);
  m->drop_chain(m->dot);
  for (auto s : m->hand_space)
    if (s) xrDestroySpace(s);
  if (m->set) xrDestroyActionSet(m->set);
  if (m->local) xrDestroySpace(m->local);
  if (m->stage) xrDestroySpace(m->stage);
  if (m->view_space) xrDestroySpace(m->view_space);
  if (m->session) xrDestroySession(m->session);
  if (m->instance) xrDestroyInstance(m->instance);
}

bool Xr::init(std::string& err) {
  const char* exts[] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
  XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
  strcpy_s(ici.applicationInfo.applicationName, "FullImmersion");
  strcpy_s(ici.applicationInfo.engineName, "FullImmersion");
  ici.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
  ici.enabledExtensionCount = 1;
  ici.enabledExtensionNames = exts;
  if (XR_FAILED(xrCreateInstance(&ici, &m->instance))) {
    err = "No OpenXR runtime answered (is SteamVR installed?)";
    return false;
  }
  XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
  xrGetInstanceProperties(m->instance, &ip);
  m->runtime = ip.runtimeName;

  XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
  sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
  XrResult r = XR_ERROR_FORM_FACTOR_UNAVAILABLE;
  for (int tries = 0; tries < 30; ++tries) {  // SteamVR may still be waking the headset
    r = xrGetSystem(m->instance, &sgi, &m->system);
    if (r != XR_ERROR_FORM_FACTOR_UNAVAILABLE) break;
    Sleep(1000);
  }
  if (XR_FAILED(r)) {
    err = "No headset found (is it on, and does SteamVR see it?)";
    return false;
  }
  XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
  xrGetSystemProperties(m->instance, m->system, &sp);
  m->max_layers = sp.graphicsProperties.maxLayerCount;

  PFN_xrGetD3D11GraphicsRequirementsKHR get_req = nullptr;
  xrGetInstanceProcAddr(m->instance, "xrGetD3D11GraphicsRequirementsKHR", reinterpret_cast<PFN_xrVoidFunction*>(&get_req));
  XrGraphicsRequirementsD3D11KHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
  if (!get_req || XR_FAILED(get_req(m->instance, m->system, &req))) {
    err = "The runtime gave no Direct3D 11 requirements";
    return false;
  }
  m->luid = req.adapterLuid;
  return true;
}

LUID Xr::adapter() const { return m->luid; }
std::string Xr::runtime_name() const { return m->runtime; }
uint32_t Xr::max_layers() const { return m->max_layers; }
float Xr::floor_y() const { return m->floor; }
bool Xr::running() const { return m->running; }

bool Xr::start(ID3D11Device* device, Renderer* renderer, std::string& err) {
  m->device = device;
  m->renderer = renderer;
  XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
  binding.device = device;
  XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
  sci.next = &binding;
  sci.systemId = m->system;
  if (XR_FAILED(xrCreateSession(m->instance, &sci, &m->session))) {
    err = "xrCreateSession failed";
    return false;
  }

  XrReferenceSpaceCreateInfo rsi{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
  rsi.poseInReferenceSpace.orientation.w = 1;
  rsi.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
  xrCreateReferenceSpace(m->session, &rsi, &m->local);
  rsi.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
  xrCreateReferenceSpace(m->session, &rsi, &m->view_space);
  uint32_t ns = 0;
  xrEnumerateReferenceSpaces(m->session, 0, &ns, nullptr);
  std::vector<XrReferenceSpaceType> spaces(ns);
  xrEnumerateReferenceSpaces(m->session, ns, &ns, spaces.data());
  if (std::find(spaces.begin(), spaces.end(), XR_REFERENCE_SPACE_TYPE_STAGE) != spaces.end()) {
    rsi.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
    xrCreateReferenceSpace(m->session, &rsi, &m->stage);
  }

  // The colour format: sRGB if offered, so the compositor reads our linear-light drawing right.
  uint32_t nf = 0;
  xrEnumerateSwapchainFormats(m->session, 0, &nf, nullptr);
  std::vector<int64_t> formats(nf);
  xrEnumerateSwapchainFormats(m->session, nf, &nf, formats.data());
  for (int64_t want : {(int64_t)DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, (int64_t)DXGI_FORMAT_R8G8B8A8_UNORM_SRGB})
    if (!m->format && std::find(formats.begin(), formats.end(), want) != formats.end()) m->format = want;
  if (!m->format) {
    err = "The runtime offers no sRGB colour swapchain";
    return false;
  }

  uint32_t nv = 0;
  xrEnumerateViewConfigurationViews(m->instance, m->system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &nv, m->eye_cfg);
  for (int e = 0; e < 2; ++e)
    if (!m->make_chain(m->eyes[e], m->eye_cfg[e].recommendedImageRectWidth, m->eye_cfg[e].recommendedImageRectHeight, err))
      return false;
  if (!m->make_chain(m->dot, 64, 64, err)) return false;

  // The controllers: a pointer, the trigger, the grip, the stick, A, and a buzz.
  XrActionSetCreateInfo asi{XR_TYPE_ACTION_SET_CREATE_INFO};
  strcpy_s(asi.actionSetName, "room");
  strcpy_s(asi.localizedActionSetName, "The room");
  xrCreateActionSet(m->instance, &asi, &m->set);
  m->hand_path[0] = m->path("/user/hand/left");
  m->hand_path[1] = m->path("/user/hand/right");
  auto action = [&](XrAction& a, const char* name, const char* label, XrActionType type) {
    XrActionCreateInfo ai{XR_TYPE_ACTION_CREATE_INFO};
    strcpy_s(ai.actionName, name);
    strcpy_s(ai.localizedActionName, label);
    ai.actionType = type;
    ai.countSubactionPaths = 2;
    ai.subactionPaths = m->hand_path;
    xrCreateAction(m->set, &ai, &a);
  };
  action(m->aim, "aim", "Point", XR_ACTION_TYPE_POSE_INPUT);
  action(m->select, "select", "Click", XR_ACTION_TYPE_BOOLEAN_INPUT);
  action(m->squeeze, "squeeze", "Grab a panel", XR_ACTION_TYPE_BOOLEAN_INPUT);
  action(m->alt, "alt", "Right-click", XR_ACTION_TYPE_BOOLEAN_INPUT);
  action(m->stick, "stick", "Scroll, or push and pull a held panel", XR_ACTION_TYPE_VECTOR2F_INPUT);
  action(m->haptic, "haptic", "Buzz", XR_ACTION_TYPE_VIBRATION_OUTPUT);

  auto suggest = [&](const char* profile, std::vector<std::pair<XrAction, const char*>> b) {
    std::vector<XrActionSuggestedBinding> sb;
    for (auto& [a, p] : b) sb.push_back({a, m->path(p)});
    XrInteractionProfileSuggestedBinding s{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    s.interactionProfile = m->path(profile);
    s.countSuggestedBindings = (uint32_t)sb.size();
    s.suggestedBindings = sb.data();
    xrSuggestInteractionProfileBindings(m->instance, &s);
  };
  suggest("/interaction_profiles/valve/index_controller",
          {{m->aim, "/user/hand/left/input/aim/pose"}, {m->aim, "/user/hand/right/input/aim/pose"},
           {m->select, "/user/hand/left/input/trigger/click"}, {m->select, "/user/hand/right/input/trigger/click"},
           {m->squeeze, "/user/hand/left/input/squeeze/value"}, {m->squeeze, "/user/hand/right/input/squeeze/value"},
           {m->alt, "/user/hand/left/input/a/click"}, {m->alt, "/user/hand/right/input/a/click"},
           {m->stick, "/user/hand/left/input/thumbstick"}, {m->stick, "/user/hand/right/input/thumbstick"},
           {m->haptic, "/user/hand/left/output/haptic"}, {m->haptic, "/user/hand/right/output/haptic"}});
  suggest("/interaction_profiles/khr/simple_controller",
          {{m->aim, "/user/hand/left/input/aim/pose"}, {m->aim, "/user/hand/right/input/aim/pose"},
           {m->select, "/user/hand/left/input/select/click"}, {m->select, "/user/hand/right/input/select/click"},
           {m->alt, "/user/hand/left/input/menu/click"}, {m->alt, "/user/hand/right/input/menu/click"},
           {m->haptic, "/user/hand/left/output/haptic"}, {m->haptic, "/user/hand/right/output/haptic"}});
  XrSessionActionSetsAttachInfo at{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
  at.countActionSets = 1;
  at.actionSets = &m->set;
  xrAttachSessionActionSets(m->session, &at);
  for (int h = 0; h < 2; ++h) {
    XrActionSpaceCreateInfo si{XR_TYPE_ACTION_SPACE_CREATE_INFO};
    si.action = m->aim;
    si.subactionPath = m->hand_path[h];
    si.poseInActionSpace.orientation.w = 1;
    xrCreateActionSpace(m->session, &si, &m->hand_space[h]);
  }
  return true;
}

bool Xr::poll() {
  XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
  while (xrPollEvent(m->instance, &ev) == XR_SUCCESS) {
    if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
      m->state = reinterpret_cast<XrEventDataSessionStateChanged*>(&ev)->state;
      if (m->state == XR_SESSION_STATE_READY) {
        XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
        bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        m->running = XR_SUCCEEDED(xrBeginSession(m->session, &bi));
      } else if (m->state == XR_SESSION_STATE_STOPPING) {
        xrEndSession(m->session);
        m->running = false;
      } else if (m->state == XR_SESSION_STATE_EXITING || m->state == XR_SESSION_STATE_LOSS_PENDING) {
        m->ended = true;
      }
    } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
      m->ended = true;
    }
    ev = {XR_TYPE_EVENT_DATA_BUFFER};
  }
  return !m->ended;
}

bool Xr::begin(XrFrame& f) {
  if (!m->running) return false;
  m->frame_state = {XR_TYPE_FRAME_STATE};
  if (XR_FAILED(xrWaitFrame(m->session, nullptr, &m->frame_state))) return false;
  if (XR_FAILED(xrBeginFrame(m->session, nullptr))) return false;
  XrTime t = m->frame_state.predictedDisplayTime;
  f.dt = m->last_time ? std::min(0.1f, float(double(t - m->last_time) * 1e-9)) : 0.0f;
  m->last_time = t;
  f.should_render = m->frame_state.shouldRender;
  m->find_floor();

  // The controllers, only while we have the focus.
  if (m->state == XR_SESSION_STATE_FOCUSED) {
    XrActiveActionSet active{m->set, XR_NULL_PATH};
    XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
    sync.countActiveActionSets = 1;
    sync.activeActionSets = &active;
    xrSyncActions(m->session, &sync);
  }
  for (int h = 0; h < 2; ++h) {
    XrHand& hand = f.hands[h];
    hand = {};
    XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
    if (m->state == XR_SESSION_STATE_FOCUSED && XR_SUCCEEDED(xrLocateSpace(m->hand_space[h], m->local, t, &loc)) &&
        (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
        (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
      hand.tracked = true;
      hand.aim = to_pose(loc.pose);
    }
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
    gi.subactionPath = m->hand_path[h];
    XrActionStateBoolean b{XR_TYPE_ACTION_STATE_BOOLEAN};
    gi.action = m->select;
    if (XR_SUCCEEDED(xrGetActionStateBoolean(m->session, &gi, &b))) hand.select = b.isActive && b.currentState;
    gi.action = m->squeeze;
    b = {XR_TYPE_ACTION_STATE_BOOLEAN};
    if (XR_SUCCEEDED(xrGetActionStateBoolean(m->session, &gi, &b))) hand.squeeze = b.isActive && b.currentState;
    gi.action = m->alt;
    b = {XR_TYPE_ACTION_STATE_BOOLEAN};
    if (XR_SUCCEEDED(xrGetActionStateBoolean(m->session, &gi, &b))) hand.alt = b.isActive && b.currentState;
    gi.action = m->stick;
    XrActionStateVector2f v{XR_TYPE_ACTION_STATE_VECTOR2F};
    if (XR_SUCCEEDED(xrGetActionStateVector2f(m->session, &gi, &v)) && v.isActive)
      hand.stick_x = v.currentState.x, hand.stick_y = v.currentState.y;
  }

  // The eyes.
  XrViewLocateInfo vli{XR_TYPE_VIEW_LOCATE_INFO};
  vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
  vli.displayTime = t;
  vli.space = m->local;
  XrViewState vs{XR_TYPE_VIEW_STATE};
  XrView views[2]{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
  uint32_t n = 0;
  if (XR_FAILED(xrLocateViews(m->session, &vli, &vs, 2, &n, views)) || n != 2) f.should_render = false;
  for (int e = 0; e < 2; ++e) {
    auto& fov = views[e].fov;
    f.eyes[e] = View::of_pose(to_pose(views[e].pose), std::tan(fov.angleLeft), std::tan(fov.angleRight),
                              std::tan(fov.angleDown), std::tan(fov.angleUp));
    f.eyes[e].floor_y = m->floor;
    m->eye_views_[e] = views[e];
  }
  XrSpaceLocation head{XR_TYPE_SPACE_LOCATION};
  if (XR_SUCCEEDED(xrLocateSpace(m->view_space, m->local, t, &head)) &&
      (head.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
    f.head = to_pose(head.pose);
  return true;
}

void Xr::end(const XrFrame& f, const std::vector<PanelDraw>& room, const std::vector<Beam>& beams,
             const std::vector<PanelLayer>& panels, const std::vector<DotLayer>& dots) {
  std::vector<XrCompositionLayerBaseHeader*> layers;
  XrCompositionLayerProjection proj{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
  XrCompositionLayerProjectionView pv[2]{{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                         {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
  std::vector<XrCompositionLayerQuad> quads;
  quads.reserve(panels.size() + dots.size());

  if (f.should_render) {
    for (int e = 0; e < 2; ++e) {
      Chain& c = m->eyes[e];
      m->write(c, [&](ID3D11RenderTargetView* rtv) { m->renderer->draw_view(f.eyes[e], rtv, c.w, c.h, room, beams); });
      pv[e].pose = m->eye_views_[e].pose;
      pv[e].fov = m->eye_views_[e].fov;
      pv[e].subImage.swapchain = c.handle;
      pv[e].subImage.imageRect = {{0, 0}, {(int32_t)c.w, (int32_t)c.h}};
    }
    proj.space = m->local;
    proj.viewCount = 2;
    proj.views = pv;
    layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&proj));

    // Windows, far to near, each in its own quad layer while the compositor has room for them.
    for (auto& [k, c] : m->panels) ++c.unused_frames;
    std::vector<const PanelLayer*> order;
    for (auto& p : panels) order.push_back(&p);
    std::sort(order.begin(), order.end(), [&](auto a, auto b) {
      return length(a->panel->centre() - f.head.position) > length(b->panel->centre() - f.head.position);
    });
    size_t budget = m->max_layers > 1 + dots.size() ? m->max_layers - 1 - dots.size() : 0;  // the caller keeps to it
    for (auto* p : order) {
      if (quads.size() >= budget || !p->picture || !p->width || !p->height) continue;
      Chain& c = m->panels[p->key];
      uint32_t w = std::min<uint32_t>(p->width, 4096), h = std::min<uint32_t>(p->height, 4096);
      bool fresh = p->fresh;
      if (c.handle && (c.w != w || c.h != h)) m->drop_chain(c);
      if (!c.handle) {
        std::string err;
        if (!m->make_chain(c, w, h, err)) continue;
        fresh = true;
      }
      c.unused_frames = 0;
      if (fresh) m->write(c, [&](ID3D11RenderTargetView* rtv) { m->renderer->blit(p->picture, rtv, c.w, c.h); });
      XrCompositionLayerQuad q{XR_TYPE_COMPOSITION_LAYER_QUAD};
      q.space = m->local;
      q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
      q.subImage.swapchain = c.handle;
      q.subImage.imageRect = {{0, 0}, {(int32_t)c.w, (int32_t)c.h}};
      q.pose = to_xr({quat_axis_angle({0, 1, 0}, -p->panel->yaw), p->panel->centre()});
      q.size = {p->panel->width, p->panel->panel_height()};
      quads.push_back(q);
    }
    // Swapchains for windows no longer shown go after a second.
    for (auto it = m->panels.begin(); it != m->panels.end();) {
      if (it->second.unused_frames > 90) {
        m->drop_chain(it->second);
        it = m->panels.erase(it);
      } else {
        ++it;
      }
    }
    // The pointers' tips, on top.
    if (!m->dot_drawn) m->dot_drawn = m->write(m->dot, [&](ID3D11RenderTargetView* rtv) { m->renderer->draw_dot(rtv, 64); });
    for (auto& d : dots) {
      XrCompositionLayerQuad q{XR_TYPE_COMPOSITION_LAYER_QUAD};
      q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;  // premultiplied, as drawn
      q.space = m->local;
      q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
      q.subImage.swapchain = m->dot.handle;
      q.subImage.imageRect = {{0, 0}, {64, 64}};
      q.pose = to_xr({quat_axis_angle({0, 1, 0}, -d.yaw), d.at});
      q.size = {0.014f, 0.014f};
      quads.push_back(q);
    }
    for (auto& q : quads) layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&q));
  }

  XrFrameEndInfo fe{XR_TYPE_FRAME_END_INFO};
  fe.displayTime = m->frame_state.predictedDisplayTime;
  fe.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
  fe.layerCount = (uint32_t)layers.size();
  fe.layers = layers.data();
  xrEndFrame(m->session, &fe);
}

void Xr::pulse(int hand, float seconds, float amplitude) {
  if (!m->running) return;
  XrHapticVibration v{XR_TYPE_HAPTIC_VIBRATION};
  v.duration = XrDuration(seconds * 1e9);
  v.frequency = XR_FREQUENCY_UNSPECIFIED;
  v.amplitude = amplitude;
  XrHapticActionInfo hi{XR_TYPE_HAPTIC_ACTION_INFO};
  hi.action = m->haptic;
  hi.subactionPath = m->hand_path[hand];
  xrApplyHapticFeedback(m->session, &hi, reinterpret_cast<XrHapticBaseHeader*>(&v));
}

}  // namespace fi
