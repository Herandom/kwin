/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 1999, 2000 Matthias Ettrich <ettrich@kde.org>
    SPDX-FileCopyrightText: 2003 Lubos Lunak <l.lunak@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
// own
#include "x11window.h"
// kwin
#include "core/output.h"
#if KWIN_BUILD_ACTIVITIES
#include "activities.h"
#endif
#include "atoms.h"
#include "client_machine.h"
#include "compositor.h"
#include "cursor.h"
#include "decorations/decoratedclient.h"
#include "decorations/decorationbridge.h"
#include "effect/effecthandler.h"
#include "focuschain.h"
#include "group.h"
#include "killprompt.h"
#include "netinfo.h"
#include "placement.h"
#include "scene/surfaceitem_x11.h"
#include "scene/windowitem.h"
#include "screenedge.h"
#include "shadow.h"
#include "tiles/tilemanager.h"
#include "virtualdesktops.h"
#include "wayland/surface.h"
#include "wayland_server.h"
#include "workspace.h"
#include <KDecoration2/DecoratedClient>
#include <KDecoration2/Decoration>
// KDE
#include <KLocalizedString>
#include <KStartupInfo>
#include <KX11Extras>
// Qt
#include <QApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMouseEvent>
#include <QPainter>
#include <QProcess>
// xcb
#include <xcb/xcb_icccm.h>
// system
#include <unistd.h>
// c++
#include <cmath>
#include <csignal>

// Put all externs before the namespace statement to allow the linker
// to resolve them properly

namespace KWin
{

static uint32_t frameEventMask()
{
    if (waylandServer()) {
        return XCB_EVENT_MASK_FOCUS_CHANGE
            | XCB_EVENT_MASK_STRUCTURE_NOTIFY
            | XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT
            | XCB_EVENT_MASK_PROPERTY_CHANGE;
    } else {
        return XCB_EVENT_MASK_FOCUS_CHANGE
            | XCB_EVENT_MASK_STRUCTURE_NOTIFY
            | XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT
            | XCB_EVENT_MASK_PROPERTY_CHANGE
            | XCB_EVENT_MASK_KEY_PRESS
            | XCB_EVENT_MASK_KEY_RELEASE
            | XCB_EVENT_MASK_ENTER_WINDOW
            | XCB_EVENT_MASK_LEAVE_WINDOW
            | XCB_EVENT_MASK_BUTTON_PRESS
            | XCB_EVENT_MASK_BUTTON_RELEASE
            | XCB_EVENT_MASK_BUTTON_MOTION
            | XCB_EVENT_MASK_POINTER_MOTION
            | XCB_EVENT_MASK_KEYMAP_STATE
            | XCB_EVENT_MASK_EXPOSURE
            | XCB_EVENT_MASK_VISIBILITY_CHANGE;
    }
}

static uint32_t wrapperEventMask()
{
    if (waylandServer()) {
        return XCB_EVENT_MASK_FOCUS_CHANGE
            | XCB_EVENT_MASK_STRUCTURE_NOTIFY
            | XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT
            | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY;
    } else {
        return XCB_EVENT_MASK_FOCUS_CHANGE
            | XCB_EVENT_MASK_STRUCTURE_NOTIFY
            | XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT
            | XCB_EVENT_MASK_KEY_PRESS
            | XCB_EVENT_MASK_KEY_RELEASE
            | XCB_EVENT_MASK_ENTER_WINDOW
            | XCB_EVENT_MASK_LEAVE_WINDOW
            | XCB_EVENT_MASK_BUTTON_PRESS
            | XCB_EVENT_MASK_BUTTON_RELEASE
            | XCB_EVENT_MASK_BUTTON_MOTION
            | XCB_EVENT_MASK_POINTER_MOTION
            | XCB_EVENT_MASK_KEYMAP_STATE
            | XCB_EVENT_MASK_EXPOSURE
            | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY;
    }
}

static uint32_t clientEventMask()
{
    if (waylandServer()) {
        return XCB_EVENT_MASK_FOCUS_CHANGE
            | XCB_EVENT_MASK_PROPERTY_CHANGE;
    } else {
        return XCB_EVENT_MASK_FOCUS_CHANGE
            | XCB_EVENT_MASK_PROPERTY_CHANGE
            | XCB_EVENT_MASK_ENTER_WINDOW
            | XCB_EVENT_MASK_LEAVE_WINDOW
            | XCB_EVENT_MASK_KEY_PRESS
            | XCB_EVENT_MASK_KEY_RELEASE;
    }
}

// window types that are supported as normal windows (i.e. KWin actually manages them)
const NET::WindowTypes SUPPORTED_MANAGED_WINDOW_TYPES_MASK = NET::NormalMask
    | NET::DesktopMask
    | NET::DockMask
    | NET::ToolbarMask
    | NET::MenuMask
    | NET::DialogMask
    /*| NET::OverrideMask*/
    | NET::TopMenuMask
    | NET::UtilityMask
    | NET::SplashMask
    | NET::NotificationMask
    | NET::OnScreenDisplayMask
    | NET::CriticalNotificationMask
    | NET::AppletPopupMask;

// window types that are supported as unmanaged (mainly for compositing)
const NET::WindowTypes SUPPORTED_UNMANAGED_WINDOW_TYPES_MASK = NET::NormalMask
    | NET::DesktopMask
    | NET::DockMask
    | NET::ToolbarMask
    | NET::MenuMask
    | NET::DialogMask
    /*| NET::OverrideMask*/
    | NET::TopMenuMask
    | NET::UtilityMask
    | NET::SplashMask
    | NET::DropdownMenuMask
    | NET::PopupMenuMask
    | NET::TooltipMask
    | NET::NotificationMask
    | NET::ComboBoxMask
    | NET::DNDIconMask
    | NET::OnScreenDisplayMask
    | NET::CriticalNotificationMask;

X11DecorationRenderer::X11DecorationRenderer(Decoration::DecoratedClientImpl *client)
    : DecorationRenderer(client)
    , m_scheduleTimer(new QTimer(this))
    , m_gc(XCB_NONE)
{
    // Delay any rendering to end of event cycle to catch multiple updates per cycle.
    m_scheduleTimer->setSingleShot(true);
    m_scheduleTimer->setInterval(0);
    connect(m_scheduleTimer, &QTimer::timeout, this, &X11DecorationRenderer::update);
    connect(this, &X11DecorationRenderer::damaged, m_scheduleTimer, static_cast<void (QTimer::*)()>(&QTimer::start));
}

X11DecorationRenderer::~X11DecorationRenderer()
{
    if (m_gc != XCB_NONE) {
        xcb_free_gc(kwinApp()->x11Connection(), m_gc);
    }
}

void X11DecorationRenderer::update()
{
    if (!damage().isEmpty()) {
        render(damage());
        resetDamage();
    }
}

void X11DecorationRenderer::render(const QRegion &region)
{
    if (!client()) {
        return;
    }
    xcb_connection_t *c = kwinApp()->x11Connection();
    X11Window *window = static_cast<X11Window *>(client()->window());

    if (m_gc == XCB_NONE) {
        m_gc = xcb_generate_id(c);
        xcb_create_gc(c, m_gc, window->frameId(), 0, nullptr);
    }

    QRectF left, top, right, bottom;
    window->layoutDecorationRects(left, top, right, bottom);

    const QRect geometry = region.boundingRect();
    left = left.intersected(geometry);
    top = top.intersected(geometry);
    right = right.intersected(geometry);
    bottom = bottom.intersected(geometry);

    auto renderPart = [this, c, window](const QRect &geo) {
        if (!geo.isValid()) {
            return;
        }

        // Guess the pixel format of the X pixmap into which the QImage will be copied.
        QImage::Format format;
        const int depth = window->depth();
        switch (depth) {
        case 30:
            format = QImage::Format_A2RGB30_Premultiplied;
            break;
        case 24:
        case 32:
            format = QImage::Format_ARGB32_Premultiplied;
            break;
        default:
            qCCritical(KWIN_CORE) << "Unsupported client depth" << depth;
            format = QImage::Format_ARGB32_Premultiplied;
            break;
        };

        QImage image(geo.width(), geo.height(), format);
        image.fill(Qt::transparent);
        QPainter p(&image);
        p.setRenderHint(QPainter::Antialiasing);
        p.setWindow(geo);
        p.setClipRect(geo);
        renderToPainter(&p, geo);

        xcb_put_image(c, XCB_IMAGE_FORMAT_Z_PIXMAP, window->frameId(), m_gc,
                      image.width(), image.height(), geo.x(), geo.y(), 0, window->depth(),
                      image.sizeInBytes(), image.constBits());
    };
    renderPart(left.toRect());
    renderPart(top.toRect());
    renderPart(right.toRect());
    renderPart(bottom.toRect());

    xcb_flush(c);
    resetImageSizesDirty();
}

// Creating a client:
//  - only by calling Workspace::createClient()
//      - it creates a new client and calls manage() for it
//
// Destroying a client:
//  - destroyWindow() - only when the window itself has been destroyed
//      - releaseWindow() - the window is kept, only the client itself is destroyed

/**
 * \class Client x11client.h
 * \brief The Client class encapsulates a window decoration frame.
 */

/**
 * This ctor is "dumb" - it only initializes data. All the real initialization
 * is done in manage().
 */
X11Window::X11Window()
    : Window()
    , m_client()
    , m_wrapper()
    , m_frame()
    , m_activityUpdatesBlocked(false)
    , m_blockedActivityUpdatesRequireTransients(false)
    , m_moveResizeGrabWindow()
    , move_resize_has_keyboard_grab(false)
    , m_managed(false)
    , m_transientForId(XCB_WINDOW_NONE)
    , m_originalTransientForId(XCB_WINDOW_NONE)
    , shade_below(nullptr)
    , m_motif(atoms->motif_wm_hints)
    , blocks_compositing(false)
    , in_group(nullptr)
    , ping_timer(nullptr)
    , m_pingTimestamp(XCB_TIME_CURRENT_TIME)
    , m_userTime(XCB_TIME_CURRENT_TIME) // Not known yet
    , allowed_actions()
    , shade_geometry_change(false)
    , sm_stacking_order(-1)
    , activitiesDefined(false)
    , sessionActivityOverride(false)
    , m_decoInputExtent()
    , m_focusOutTimer(nullptr)
{
    // TODO: Do all as initialization
    m_syncRequest.counter = m_syncRequest.alarm = XCB_NONE;
    m_syncRequest.timeout = m_syncRequest.failsafeTimeout = nullptr;
    m_syncRequest.lastTimestamp = xTime();
    m_syncRequest.isPending = false;
    m_syncRequest.interactiveResize = false;

    // Set the initial mapping state
    mapping_state = Withdrawn;

    info = nullptr;

    m_fullscreenMode = FullScreenNone;
    noborder = false;
    app_noborder = false;
    ignore_focus_stealing = false;
    check_active_modal = false;

    max_mode = MaximizeRestore;

    connect(clientMachine(), &ClientMachine::localhostChanged, this, &X11Window::updateCaption);
    connect(options, &Options::configChanged, this, &X11Window::updateMouseGrab);
    connect(options, &Options::condensedTitleChanged, this, &X11Window::updateCaption);
    connect(this, &X11Window::shapeChanged, this, &X11Window::discardShapeRegion);

    if (kwinApp()->operationMode() == Application::OperationModeX11) {
        connect(this, &X11Window::moveResizeCursorChanged, this, [this](CursorShape cursor) {
            xcb_cursor_t nativeCursor = Cursors::self()->mouse()->x11Cursor(cursor);
            m_frame.defineCursor(nativeCursor);
            if (m_decoInputExtent.isValid()) {
                m_decoInputExtent.defineCursor(nativeCursor);
            }
            if (isInteractiveMoveResize()) {
                // changing window attributes doesn't change cursor if there's pointer grab active
                xcb_change_active_pointer_grab(kwinApp()->x11Connection(), nativeCursor, xTime(),
                                               XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_POINTER_MOTION | XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_LEAVE_WINDOW);
            }
        });
    }

    m_releaseTimer.setSingleShot(true);
    connect(&m_releaseTimer, &QTimer::timeout, this, [this]() {
        releaseWindow();
    });

    // SELI TODO: Initialize xsizehints??
}

/**
 * "Dumb" destructor.
 */
X11Window::~X11Window()
{
    delete info;

    if (m_killPrompt) {
        m_killPrompt->quit();
    }

    Q_ASSERT(!isInteractiveMoveResize());
    Q_ASSERT(!check_active_modal);
    Q_ASSERT(m_blockGeometryUpdates == 0);
}

std::unique_ptr<WindowItem> X11Window::createItem(Item *parentItem)
{
    return std::make_unique<WindowItemX11>(this, parentItem);
}

// Use destroyWindow() or releaseWindow(), Client instances cannot be deleted directly
void X11Window::deleteClient(X11Window *c)
{
    delete c;
}

bool X11Window::hasScheduledRelease() const
{
    return m_releaseTimer.isActive();
}

/**
 * Releases the window. The client has done its job and the window is still existing.
 */
void X11Window::releaseWindow(bool on_shutdown)
{
    destroyWindowManagementInterface();
    if (SurfaceItemX11 *item = qobject_cast<SurfaceItemX11 *>(surfaceItem())) {
        item->destroyDamage();
    }

    markAsDeleted();
    Q_EMIT closed();

    if (isUnmanaged()) {
        m_releaseTimer.stop();
        if (!findInternalWindow()) { // don't affect our own windows
            if (Xcb::Extensions::self()->isShapeAvailable()) {
                xcb_shape_select_input(kwinApp()->x11Connection(), window(), false);
            }
            Xcb::selectInput(window(), XCB_EVENT_MASK_NO_EVENT);
        }
        workspace()->removeUnmanaged(this);
    } else {
        cleanTabBox();
        if (isInteractiveMoveResize()) {
            Q_EMIT interactiveMoveResizeFinished();
        }
        setTile(nullptr);
        workspace()->rulebook()->discardUsed(this, true); // Remove ForceTemporarily rules
        StackingUpdatesBlocker blocker(workspace());
        stopDelayedInteractiveMoveResize();
        if (isInteractiveMoveResize()) {
            leaveInteractiveMoveResize();
        }
        finishWindowRules();
        // Grab X during the release to make removing of properties, setting to withdrawn state
        // and repareting to root an atomic operation (https://lists.kde.org/?l=kde-devel&m=116448102901184&w=2)
        grabXServer();
        exportMappingState(XCB_ICCCM_WM_STATE_WITHDRAWN);
        if (!on_shutdown) {
            workspace()->activateNextWindow(this);
        }
        m_frame.unmap(); // Destroying decoration would cause ugly visual effect
        cleanGrouping();
        workspace()->removeX11Window(this);
        if (!on_shutdown) {
            // Only when the window is being unmapped, not when closing down KWin (NETWM sections 5.5,5.7)
            info->setDesktop(0);
            info->setState(NET::States(), info->state()); // Reset all state flags
        }
        if (WinInfo *cinfo = dynamic_cast<WinInfo *>(info)) {
            cinfo->disable();
        }
        xcb_connection_t *c = kwinApp()->x11Connection();
        m_client.deleteProperty(atoms->kde_net_wm_user_creation_time);
        m_client.deleteProperty(atoms->net_frame_extents);
        m_client.deleteProperty(atoms->kde_net_wm_frame_strut);
        const QPointF grav = calculateGravitation(true);
        m_client.reparent(kwinApp()->x11RootWindow(), grav.x(), grav.y());
        xcb_change_save_set(c, XCB_SET_MODE_DELETE, m_client);
        m_client.selectInput(XCB_EVENT_MASK_NO_EVENT);
        if (on_shutdown) {
            // Map the window, so it can be found after another WM is started
            m_client.map();
            // TODO: Preserve minimized, shaded etc. state?
        } else { // Make sure it's not mapped if the app unmapped it (#65279). The app
            // may do map+unmap before we initially map the window by calling rawShow() from manage().
            m_client.unmap();
        }
        m_client.reset();
        m_wrapper.reset();
        m_frame.reset();
        ungrabXServer();
    }
    if (m_syncRequest.alarm != XCB_NONE) {
        xcb_sync_destroy_alarm(kwinApp()->x11Connection(), m_syncRequest.alarm);
        m_syncRequest.alarm = XCB_NONE;
    }
    unblockCompositing();
    unref();
}

/**
 * Like releaseWindow(), but this one is called when the window has been already destroyed
 * (E.g. The application closed it)
 */
void X11Window::destroyWindow()
{
    destroyWindowManagementInterface();
    if (SurfaceItemX11 *item = qobject_cast<SurfaceItemX11 *>(surfaceItem())) {
        item->forgetDamage();
    }

    markAsDeleted();
    Q_EMIT closed();

    if (isUnmanaged()) {
        m_releaseTimer.stop();
        workspace()->removeUnmanaged(this);
    } else {
        cleanTabBox();
        if (isInteractiveMoveResize()) {
            Q_EMIT interactiveMoveResizeFinished();
        }
        setTile(nullptr);
        workspace()->rulebook()->discardUsed(this, true); // Remove ForceTemporarily rules
        StackingUpdatesBlocker blocker(workspace());
        stopDelayedInteractiveMoveResize();
        if (isInteractiveMoveResize()) {
            leaveInteractiveMoveResize();
        }
        finishWindowRules();
        workspace()->activateNextWindow(this);
        cleanGrouping();
        workspace()->removeX11Window(this);
        if (WinInfo *cinfo = dynamic_cast<WinInfo *>(info)) {
            cinfo->disable();
        }
        m_client.reset(); // invalidate
        m_wrapper.reset();
        m_frame.reset();
    }
    if (m_syncRequest.alarm != XCB_NONE) {
        xcb_sync_destroy_alarm(kwinApp()->x11Connection(), m_syncRequest.alarm);
        m_syncRequest.alarm = XCB_NONE;
    }

    unblockCompositing();
    unref();
}

bool X11Window::track(xcb_window_t w)
{
    XServerGrabber xserverGrabber;
    Xcb::WindowAttributes attr(w);
    Xcb::WindowGeometry geo(w);
    if (attr.isNull() || attr->map_state != XCB_MAP_STATE_VIEWABLE) {
        return false;
    }
    if (attr->_class == XCB_WINDOW_CLASS_INPUT_ONLY) {
        return false;
    }
    if (geo.isNull()) {
        return false;
    }

    m_unmanaged = true;

    m_frame.reset(w, false);
    m_wrapper.reset(w, false);
    m_client.reset(w, false);

    Xcb::selectInput(w, attr->your_event_mask | XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_PROPERTY_CHANGE);
    m_bufferGeometry = geo.rect();
    m_frameGeometry = geo.rect();
    m_clientGeometry = geo.rect();
    checkOutput();
    m_visual = attr->visual;
    bit_depth = geo->depth;
    info = new NETWinInfo(kwinApp()->x11Connection(), w, kwinApp()->x11RootWindow(),
                          NET::WMWindowType | NET::WMPid,
                          NET::WM2Opacity | NET::WM2WindowRole | NET::WM2WindowClass | NET::WM2OpaqueRegion);
    setOpacity(info->opacityF());
    getResourceClass();
    getWmClientLeader();
    getWmClientMachine();
    if (Xcb::Extensions::self()->isShapeAvailable()) {
        xcb_shape_select_input(kwinApp()->x11Connection(), w, true);
    }
    detectShape();
    getWmOpaqueRegion();
    getSkipCloseAnimation();
    updateShadow();
    setupCompositing();
    if (QWindow *internalWindow = findInternalWindow()) {
        m_outline = internalWindow->property("__kwin_outline").toBool();
    }
    if (effects) {
        effects->checkInputWindowStacking();
    }

    switch (kwinApp()->operationMode()) {
    case Application::OperationModeXwayland:
        // The wayland surface is associated with the override-redirect window asynchronously.
        if (surface()) {
            associate();
        } else {
            connect(this, &Window::surfaceChanged, this, &X11Window::associate);
        }
        break;
    case Application::OperationModeX11:
        // We have no way knowing whether the override-redirect window can be painted. Mark it
        // as ready for painting after synthetic 50ms delay.
        QTimer::singleShot(50, this, &X11Window::setReadyForPainting);
        break;
    case Application::OperationModeWaylandOnly:
        Q_UNREACHABLE();
    }

    return true;
}

/**
 * Manages the clients. This means handling the very first maprequest:
 * reparenting, initial geometry, initial state, placement, etc.
 * Returns false if KWin is not going to manage this window.
 */
bool X11Window::manage(xcb_window_t w, bool isMapped)
{
    StackingUpdatesBlocker stacking_blocker(workspace());

    Xcb::WindowAttributes attr(w);
    Xcb::WindowGeometry windowGeometry(w);
    if (attr.isNull() || windowGeometry.isNull()) {
        return false;
    }

    // From this place on, manage() must not return false
    blockGeometryUpdates();

    embedClient(w, attr->visual, attr->colormap, windowGeometry->depth);

    m_visual = attr->visual;
    bit_depth = windowGeometry->depth;

    // SELI TODO: Order all these things in some sane manner

    const NET::Properties properties =
        NET::WMDesktop | NET::WMState | NET::WMWindowType | NET::WMStrut | NET::WMName | NET::WMIconGeometry | NET::WMIcon | NET::WMPid | NET::WMIconName;
    const NET::Properties2 properties2 =
        NET::WM2BlockCompositing | NET::WM2WindowClass | NET::WM2WindowRole | NET::WM2UserTime | NET::WM2StartupId | NET::WM2ExtendedStrut | NET::WM2Opacity | NET::WM2FullscreenMonitors | NET::WM2GroupLeader | NET::WM2Urgency | NET::WM2Input | NET::WM2Protocols | NET::WM2InitialMappingState | NET::WM2IconPixmap | NET::WM2OpaqueRegion | NET::WM2DesktopFileName | NET::WM2GTKFrameExtents | NET::WM2GTKApplicationId;

    auto wmClientLeaderCookie = fetchWmClientLeader();
    auto skipCloseAnimationCookie = fetchSkipCloseAnimation();
    auto showOnScreenEdgeCookie = fetchShowOnScreenEdge();
    auto colorSchemeCookie = fetchPreferredColorScheme();
    auto transientCookie = fetchTransient();
    auto activitiesCookie = fetchActivities();
    auto applicationMenuServiceNameCookie = fetchApplicationMenuServiceName();
    auto applicationMenuObjectPathCookie = fetchApplicationMenuObjectPath();

    m_geometryHints.init(window());
    m_motif.init(window());
    info = new WinInfo(this, m_client, kwinApp()->x11RootWindow(), properties, properties2);

    if (isDesktop() && bit_depth == 32) {
        // force desktop windows to be opaque. It's a desktop after all, there is no window below
        bit_depth = 24;
    }

    // If it's already mapped, ignore hint
    bool init_minimize = !isMapped && (info->initialMappingState() == NET::Iconic);

    getResourceClass();
    readWmClientLeader(wmClientLeaderCookie);
    getWmClientMachine();
    getSyncCounter();
    setCaption(readName());

    setupWindowRules();
    connect(this, &X11Window::windowClassChanged, this, &X11Window::evaluateWindowRules);

    if (Xcb::Extensions::self()->isShapeAvailable()) {
        xcb_shape_select_input(kwinApp()->x11Connection(), window(), true);
    }
    detectShape();
    detectNoBorder();
    fetchIconicName();
    setClientFrameExtents(info->gtkFrameExtents());

    // Needs to be done before readTransient() because of reading the group
    checkGroup();
    updateUrgency();
    updateAllowedActions(); // Group affects isMinimizable()

    setModal((info->state() & NET::Modal) != 0); // Needs to be valid before handling groups
    readTransientProperty(transientCookie);
    QString desktopFileName = QString::fromUtf8(info->desktopFileName());
    if (desktopFileName.isEmpty()) {
        desktopFileName = QString::fromUtf8(info->gtkApplicationId());
    }
    setDesktopFileName(rules()->checkDesktopFile(desktopFileName, true));
    getIcons();
    connect(this, &X11Window::desktopFileNameChanged, this, &X11Window::getIcons);

    m_geometryHints.read();
    getMotifHints();
    getWmOpaqueRegion();
    readSkipCloseAnimation(skipCloseAnimationCookie);
    updateShadow();

    // TODO: Try to obey all state information from info->state()

    setOriginalSkipTaskbar((info->state() & NET::SkipTaskbar) != 0);
    setSkipPager((info->state() & NET::SkipPager) != 0);
    setSkipSwitcher((info->state() & NET::SkipSwitcher) != 0);

    setupCompositing();

    KStartupInfoId asn_id;
    KStartupInfoData asn_data;
    bool asn_valid = workspace()->checkStartupNotification(window(), asn_id, asn_data);

    // Make sure that the input window is created before we update the stacking order
    updateInputWindow();
    updateLayer();

    SessionInfo *session = workspace()->sessionManager()->takeSessionInfo(this);
    if (session) {
        init_minimize = session->minimized;
        noborder = session->noBorder;
    }

    setShortcut(rules()->checkShortcut(session ? session->shortcut : QString(), true));

    init_minimize = rules()->checkMinimize(init_minimize, !isMapped);
    noborder = rules()->checkNoBorder(noborder, !isMapped);

    readActivities(activitiesCookie);

    // Initial desktop placement
    std::optional<QList<VirtualDesktop *>> initialDesktops;
    if (session) {
        if (session->onAllDesktops) {
            initialDesktops = QList<VirtualDesktop *>{};
        } else {
            VirtualDesktop *desktop = VirtualDesktopManager::self()->desktopForX11Id(session->desktop);
            if (desktop) {
                initialDesktops = QList<VirtualDesktop *>{desktop};
            }
        }
        setOnActivities(session->activities);
    } else {
        // If this window is transient, ensure that it is opened on the
        // same window as its parent.  this is necessary when an application
        // starts up on a different desktop than is currently displayed
        if (isTransient()) {
            auto mainwindows = mainWindows();
            bool on_current = false;
            bool on_all = false;
            Window *maincl = nullptr;
            // This is slightly duplicated from Placement::placeOnMainWindow()
            for (auto it = mainwindows.constBegin(); it != mainwindows.constEnd(); ++it) {
                if (mainwindows.count() > 1 && // A group-transient
                    (*it)->isSpecialWindow() && // Don't consider toolbars etc when placing
                    !(info->state() & NET::Modal)) { // except when it's modal (blocks specials as well)
                    continue;
                }
                maincl = *it;
                if ((*it)->isOnCurrentDesktop()) {
                    on_current = true;
                }
                if ((*it)->isOnAllDesktops()) {
                    on_all = true;
                }
            }
            if (on_all) {
                initialDesktops = QList<VirtualDesktop *>{};
            } else if (on_current) {
                initialDesktops = QList<VirtualDesktop *>{VirtualDesktopManager::self()->currentDesktop()};
            } else if (maincl) {
                initialDesktops = maincl->desktops();
            }

            if (maincl) {
                setOnActivities(maincl->activities());
            }
        } else { // a transient shall appear on its leader and not drag that around
            int desktopId = 0;
            if (info->desktop()) {
                desktopId = info->desktop(); // Window had the initial desktop property, force it
            }
            if (desktopId == 0 && asn_valid && asn_data.desktop() != 0) {
                desktopId = asn_data.desktop();
            }
            if (desktopId) {
                if (desktopId == NET::OnAllDesktops) {
                    initialDesktops = QList<VirtualDesktop *>{};
                } else {
                    VirtualDesktop *desktop = VirtualDesktopManager::self()->desktopForX11Id(desktopId);
                    if (desktop) {
                        initialDesktops = QList<VirtualDesktop *>{desktop};
                    }
                }
            }
        }
#if KWIN_BUILD_ACTIVITIES
        if (Workspace::self()->activities() && !isMapped && !skipTaskbar() && isNormalWindow() && !activitiesDefined) {
            // a new, regular window, when we're not recovering from a crash,
            // and it hasn't got an activity. let's try giving it the current one.
            // TODO: decide whether to keep this before the 4.6 release
            // TODO: if we are keeping it (at least as an option), replace noborder checking
            // with a public API for setting windows to be on all activities.
            // something like KWindowSystem::setOnAllActivities or
            // KActivityConsumer::setOnAllActivities
            setOnActivity(Workspace::self()->activities()->current(), true);
        }
#endif
    }

    // If initialDesktops has no value, it means that the client doesn't prefer any
    // desktop so place it on the current virtual desktop.
    if (!initialDesktops.has_value()) {
        if (isDesktop()) {
            initialDesktops = QList<VirtualDesktop *>{};
        } else {
            initialDesktops = QList<VirtualDesktop *>{VirtualDesktopManager::self()->currentDesktop()};
        }
    }
    setDesktops(rules()->checkDesktops(*initialDesktops, !isMapped));
    info->setDesktop(desktopId());
    workspace()->updateOnAllDesktopsOfTransients(this); // SELI TODO
    // onAllDesktopsChange(); // Decoration doesn't exist here yet

    QStringList activitiesList;
    activitiesList = rules()->checkActivity(activitiesList, !isMapped);
    if (!activitiesList.isEmpty()) {
        setOnActivities(activitiesList);
    }

    QRectF geom = session ? session->geometry : windowGeometry.rect();
    bool placementDone = false;

    QRectF area;
    bool partial_keep_in_area = isMapped || session;
    if (isMapped || session) {
        area = workspace()->clientArea(FullArea, this, geom.center());
        checkOffscreenPosition(&geom, area);
    } else {
        Output *output = nullptr;
        if (asn_data.xinerama() != -1) {
            output = workspace()->xineramaIndexToOutput(asn_data.xinerama());
        }
        if (!output) {
            output = workspace()->activeOutput();
        }
        output = rules()->checkOutput(output, !isMapped);
        area = workspace()->clientArea(PlacementArea, this, output->geometry().center());
    }

    if (isDesktop()) {
        // KWin doesn't manage desktop windows
        placementDone = true;
    }

    bool usePosition = false;
    if (isMapped || session || placementDone) {
        placementDone = true; // Use geometry
    } else if (isTransient() && !isUtility() && !isDialog() && !isSplash()) {
        usePosition = true;
    } else if (isTransient() && !hasNETSupport()) {
        usePosition = true;
    } else if (isDialog() && hasNETSupport()) {
        // If the dialog is actually non-NETWM transient window, don't try to apply placement to it,
        // it breaks with too many things (xmms, display)
        if (mainWindows().count() >= 1) {
#if 1
            // #78082 - Ok, it seems there are after all some cases when an application has a good
            // reason to specify a position for its dialog. Too bad other WMs have never bothered
            // with placement for dialogs, so apps always specify positions for their dialogs,
            // including such silly positions like always centered on the screen or under mouse.
            // Using ignoring requested position in window-specific settings helps, and now
            // there's also _NET_WM_FULL_PLACEMENT.
            usePosition = true;
#else
            ; // Force using placement policy
#endif
        } else {
            usePosition = true;
        }
    } else if (isSplash()) {
        ; // Force using placement policy
    } else {
        usePosition = true;
    }
    if (!rules()->checkIgnoreGeometry(!usePosition, true)) {
        if (m_geometryHints.hasPosition()) {
            placementDone = true;
            // Disobey xinerama placement option for now (#70943)
            area = workspace()->clientArea(PlacementArea, this, geom.center());
        }
    }

    if (isMovable() && (geom.x() > area.right() || geom.y() > area.bottom())) {
        placementDone = false; // Weird, do not trust.
    }

    if (placementDone) {
        QPointF position = geom.topLeft();
        // Session contains the position of the frame geometry before gravitating.
        if (!session) {
            position = clientPosToFramePos(position);
        }
        move(position);
    }

    // Create client group if the window will have a decoration
    bool dontKeepInArea = false;
    setColorScheme(readPreferredColorScheme(colorSchemeCookie));

    readApplicationMenuServiceName(applicationMenuServiceNameCookie);
    readApplicationMenuObjectPath(applicationMenuObjectPathCookie);

    updateDecoration(false); // Also gravitates
    // TODO: Is CentralGravity right here, when resizing is done after gravitating?
    const QSizeF constrainedClientSize = constrainClientSize(geom.size());
    resize(rules()->checkSize(clientSizeToFrameSize(constrainedClientSize), !isMapped));

    QPointF forced_pos = rules()->checkPositionSafe(invalidPoint, !isMapped);
    if (forced_pos != invalidPoint) {
        move(forced_pos);
        placementDone = true;
        // Don't keep inside workarea if the window has specially configured position
        partial_keep_in_area = true;
        area = workspace()->clientArea(FullArea, this, geom.center());
    }
    if (!placementDone) {
        // Placement needs to be after setting size
        workspace()->placement()->place(this, area);
        // The client may have been moved to another screen, update placement area.
        area = workspace()->clientArea(PlacementArea, this, moveResizeOutput());
        dontKeepInArea = true;
        placementDone = true;
    }

    // bugs #285967, #286146, #183694
    // geometry() now includes the requested size and the decoration and is at the correct screen/position (hopefully)
    // Maximization for oversized windows must happen NOW.
    // If we effectively pass keepInArea(), the window will resizeWithChecks() - i.e. constrained
    // to the combo of all screen MINUS all struts on the edges
    // If only one screen struts, this will affect screens as a side-effect, the window is artificailly shrinked
    // below the screen size and as result no more maximized what breaks KMainWindow's stupid width+1, height+1 hack
    // TODO: get KMainWindow a correct state storage what will allow to store the restore size as well.

    if (!session) { // has a better handling of this
        setGeometryRestore(moveResizeGeometry()); // Remember restore geometry
        if (isMaximizable() && (width() >= area.width() || height() >= area.height())) {
            // Window is too large for the screen, maximize in the
            // directions necessary
            const QSizeF ss = workspace()->clientArea(ScreenArea, this, area.center()).size();
            const QRectF fsa = workspace()->clientArea(FullArea, this, geom.center());
            const QSizeF cs = clientSize();
            int pseudo_max = ((info->state() & NET::MaxVert) ? MaximizeVertical : 0) | ((info->state() & NET::MaxHoriz) ? MaximizeHorizontal : 0);
            if (width() >= area.width()) {
                pseudo_max |= MaximizeHorizontal;
            }
            if (height() >= area.height()) {
                pseudo_max |= MaximizeVertical;
            }

            // heuristics:
            // if decorated client is smaller than the entire screen, the user might want to move it around (multiscreen)
            // in this case, if the decorated client is bigger than the screen (+1), we don't take this as an
            // attempt for maximization, but just constrain the size (the window simply wants to be bigger)
            // NOTICE
            // i intended a second check on cs < area.size() ("the managed client ("minus border") is smaller
            // than the workspace") but gtk / gimp seems to store it's size including the decoration,
            // thus a former maximized window wil become non-maximized
            bool keepInFsArea = false;
            if (width() < fsa.width() && (cs.width() > ss.width() + 1)) {
                pseudo_max &= ~MaximizeHorizontal;
                keepInFsArea = true;
            }
            if (height() < fsa.height() && (cs.height() > ss.height() + 1)) {
                pseudo_max &= ~MaximizeVertical;
                keepInFsArea = true;
            }

            if (pseudo_max != MaximizeRestore) {
                maximize((MaximizeMode)pseudo_max);
                // from now on, care about maxmode, since the maximization call will override mode for fix aspects
                dontKeepInArea |= (max_mode == MaximizeFull);
                QRectF savedGeometry; // Use placement when unmaximizing ...
                if (!(max_mode & MaximizeVertical)) {
                    savedGeometry.setY(y()); // ...but only for horizontal direction
                    savedGeometry.setHeight(height());
                }
                if (!(max_mode & MaximizeHorizontal)) {
                    savedGeometry.setX(x()); // ...but only for vertical direction
                    savedGeometry.setWidth(width());
                }
                setGeometryRestore(savedGeometry);
            }
            if (keepInFsArea) {
                moveResize(keepInArea(moveResizeGeometry(), fsa, partial_keep_in_area));
            }
        }
    }

    if ((!isSpecialWindow() || isToolbar()) && isMovable() && !dontKeepInArea) {
        moveResize(keepInArea(moveResizeGeometry(), area, partial_keep_in_area));
    }

    updateShape();

    // CT: Extra check for stupid jdk 1.3.1. But should make sense in general
    // if client has initial state set to Iconic and is transient with a parent
    // window that is not Iconic, set init_state to Normal
    if (init_minimize && isTransient()) {
        auto mainwindows = mainWindows();
        for (auto it = mainwindows.constBegin(); it != mainwindows.constEnd(); ++it) {
            if ((*it)->isShown()) {
                init_minimize = false; // SELI TODO: Even e.g. for NET::Utility?
            }
        }
    }
    // If a dialog is shown for minimized window, minimize it too
    if (!init_minimize && isTransient() && mainWindows().count() > 0 && workspace()->sessionManager()->state() != SessionState::Saving) {
        bool visible_parent = false;
        // Use allMainWindows(), to include also main clients of group transients
        // that have been optimized out in X11Window::checkGroupTransients()
        auto mainwindows = allMainWindows();
        for (auto it = mainwindows.constBegin(); it != mainwindows.constEnd(); ++it) {
            if ((*it)->isShown()) {
                visible_parent = true;
            }
        }
        if (!visible_parent) {
            init_minimize = true;
            demandAttention();
        }
    }

    setMinimized(init_minimize);

    // Other settings from the previous session
    if (session) {
        // Session restored windows are not considered to be new windows WRT rules,
        // I.e. obey only forcing rules
        setKeepAbove(session->keepAbove);
        setKeepBelow(session->keepBelow);
        setOriginalSkipTaskbar(session->skipTaskbar);
        setSkipPager(session->skipPager);
        setSkipSwitcher(session->skipSwitcher);
        setShade(session->shaded ? ShadeNormal : ShadeNone);
        setOpacity(session->opacity);
        setGeometryRestore(session->restore);
        if (session->maximized != MaximizeRestore) {
            maximize(MaximizeMode(session->maximized));
        }
        if (session->fullscreen != FullScreenNone) {
            setFullScreen(true);
            setFullscreenGeometryRestore(session->fsrestore);
        }
        QRectF checkedGeometryRestore = geometryRestore();
        checkOffscreenPosition(&checkedGeometryRestore, area);
        setGeometryRestore(checkedGeometryRestore);
        QRectF checkedFullscreenGeometryRestore = fullscreenGeometryRestore();
        checkOffscreenPosition(&checkedFullscreenGeometryRestore, area);
        setFullscreenGeometryRestore(checkedFullscreenGeometryRestore);
    } else {
        // Window may want to be maximized
        // done after checking that the window isn't larger than the workarea, so that
        // the restore geometry from the checks above takes precedence, and window
        // isn't restored larger than the workarea
        MaximizeMode maxmode = static_cast<MaximizeMode>(
            ((info->state() & NET::MaxVert) ? MaximizeVertical : 0) | ((info->state() & NET::MaxHoriz) ? MaximizeHorizontal : 0));
        MaximizeMode forced_maxmode = rules()->checkMaximize(maxmode, !isMapped);

        // Either hints were set to maximize, or is forced to maximize,
        // or is forced to non-maximize and hints were set to maximize
        if (forced_maxmode != MaximizeRestore || maxmode != MaximizeRestore) {
            maximize(forced_maxmode);
        }

        // Read other initial states
        setShade(rules()->checkShade(info->state() & NET::Shaded ? ShadeNormal : ShadeNone, !isMapped));
        setKeepAbove(rules()->checkKeepAbove(info->state() & NET::KeepAbove, !isMapped));
        setKeepBelow(rules()->checkKeepBelow(info->state() & NET::KeepBelow, !isMapped));
        setOriginalSkipTaskbar(rules()->checkSkipTaskbar(info->state() & NET::SkipTaskbar, !isMapped));
        setSkipPager(rules()->checkSkipPager(info->state() & NET::SkipPager, !isMapped));
        setSkipSwitcher(rules()->checkSkipSwitcher(info->state() & NET::SkipSwitcher, !isMapped));
        if (info->state() & NET::DemandsAttention) {
            demandAttention();
        }
        if (info->state() & NET::Modal) {
            setModal(true);
        }
        setOpacity(info->opacityF());

        setFullScreen(rules()->checkFullScreen(info->state() & NET::FullScreen, !isMapped));
    }

    updateAllowedActions(true);

    // Set initial user time directly
    m_userTime = readUserTimeMapTimestamp(asn_valid ? &asn_id : nullptr, asn_valid ? &asn_data : nullptr, session);
    group()->updateUserTime(m_userTime); // And do what X11Window::updateUserTime() does

    // This should avoid flicker, because real restacking is done
    // only after manage() finishes because of blocking, but the window is shown sooner
    m_frame.lower();
    if (session && session->stackingOrder != -1) {
        sm_stacking_order = session->stackingOrder;
        workspace()->restoreSessionStackingOrder(this);
    }

    if (Compositor::compositing()) {
        // Sending ConfigureNotify is done when setting mapping state below,
        // Getting the first sync response means window is ready for compositing
        sendSyncRequest();
    } else {
        ready_for_painting = true; // set to true in case compositing is turned on later. bug #160393
    }

    if (isShown()) {
        bool allow;
        if (session) {
            allow = session->active && (!workspace()->wasUserInteraction() || workspace()->activeWindow() == nullptr || workspace()->activeWindow()->isDesktop());
        } else {
            allow = allowWindowActivation(userTime(), false);
        }

        const bool isSessionSaving = workspace()->sessionManager()->state() == SessionState::Saving;

        // If session saving, force showing new windows (i.e. "save file?" dialogs etc.)
        // also force if activation is allowed
        if (!isOnCurrentDesktop() && !isMapped && !session && (allow || isSessionSaving)) {
            VirtualDesktopManager::self()->setCurrent(desktopId());
        }

        // If the window is on an inactive activity during session saving, temporarily force it to show.
        if (!isMapped && !session && isSessionSaving && !isOnCurrentActivity()) {
            setSessionActivityOverride(true);
            const auto windows = mainWindows();
            for (Window *w : windows) {
                if (X11Window *mw = dynamic_cast<X11Window *>(w)) {
                    mw->setSessionActivityOverride(true);
                }
            }
        }

        if (isOnCurrentDesktop() && !isMapped && !allow && (!session || session->stackingOrder < 0)) {
            workspace()->restackWindowUnderActive(this);
        }

        updateVisibility();

        if (!isMapped) {
            if (allow && isOnCurrentDesktop()) {
                if (!isSpecialWindow()) {
                    if (options->focusPolicyIsReasonable() && wantsTabFocus()) {
                        workspace()->requestFocus(this);
                    }
                }
            } else if (!session && !isSpecialWindow()) {
                demandAttention();
            }
        }
    } else {
        updateVisibility();
    }
    Q_ASSERT(mapping_state != Withdrawn);
    m_managed = true;
    blockGeometryUpdates(false);

    if (m_userTime == XCB_TIME_CURRENT_TIME || m_userTime == -1U) {
        // No known user time, set something old
        m_userTime = xTime() - 1000000;
        if (m_userTime == XCB_TIME_CURRENT_TIME || m_userTime == -1U) { // Let's be paranoid
            m_userTime = xTime() - 1000000 + 10;
        }
    }

    // sendSyntheticConfigureNotify(); // Done when setting mapping state

    delete session;

    applyWindowRules(); // Just in case
    workspace()->rulebook()->discardUsed(this, false); // Remove ApplyNow rules
    updateWindowRules(Rules::All); // Was blocked while !isManaged()

    setBlockingCompositing(info->isBlockingCompositing());
    readShowOnScreenEdge(showOnScreenEdgeCookie);

    setupWindowManagementInterface();

    // Forward all opacity values to the frame in case there'll be other CM running.
    connect(Compositor::self(), &Compositor::compositingToggled, this, [this](bool active) {
        if (active) {
            return;
        }
        if (opacity() == 1.0) {
            return;
        }
        NETWinInfo info(kwinApp()->x11Connection(), frameId(), kwinApp()->x11RootWindow(), NET::Properties(), NET::Properties2());
        info.setOpacityF(opacity());
    });

    switch (kwinApp()->operationMode()) {
    case Application::OperationModeXwayland:
        // The wayland surface is associated with the window asynchronously.
        if (surface()) {
            associate();
        } else {
            connect(this, &Window::surfaceChanged, this, &X11Window::associate);
        }
        connect(kwinApp(), &Application::xwaylandScaleChanged, this, &X11Window::handleXwaylandScaleChanged);
        break;
    case Application::OperationModeX11:
        break;
    case Application::OperationModeWaylandOnly:
        Q_UNREACHABLE();
    }

    return true;
}

// Called only from manage()
void X11Window::embedClient(xcb_window_t w, xcb_visualid_t visualid, xcb_colormap_t colormap, uint8_t depth)
{
    Q_ASSERT(m_client == XCB_WINDOW_NONE);
    Q_ASSERT(frameId() == XCB_WINDOW_NONE);
    Q_ASSERT(m_wrapper == XCB_WINDOW_NONE);
    m_client.reset(w, false);

    const uint32_t zero_value = 0;

    xcb_connection_t *conn = kwinApp()->x11Connection();

    // We don't want the window to be destroyed when we quit
    xcb_change_save_set(conn, XCB_SET_MODE_INSERT, m_client);

    m_client.selectInput(zero_value);
    m_client.unmap();
    m_client.setBorderWidth(zero_value);

    // Note: These values must match the order in the xcb_cw_t enum
    const uint32_t cw_values[] = {
        0, // back_pixmap
        0, // border_pixel
        colormap, // colormap
        Cursors::self()->mouse()->x11Cursor(Qt::ArrowCursor)};

    const uint32_t cw_mask = XCB_CW_BACK_PIXMAP
        | XCB_CW_BORDER_PIXEL
        | XCB_CW_COLORMAP
        | XCB_CW_CURSOR;

    // Create the frame window
    xcb_window_t frame = xcb_generate_id(conn);
    xcb_create_window(conn, depth, frame, kwinApp()->x11RootWindow(), 0, 0, 1, 1, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, visualid, cw_mask, cw_values);
    m_frame.reset(frame);

    // Create the wrapper window
    xcb_window_t wrapperId = xcb_generate_id(conn);
    xcb_create_window(conn, depth, wrapperId, frame, 0, 0, 1, 1, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, visualid, cw_mask, cw_values);
    m_wrapper.reset(wrapperId);

    m_client.reparent(m_wrapper);

    // We could specify the event masks when we create the windows, but the original
    // Xlib code didn't.  Let's preserve that behavior here for now so we don't end up
    // receiving any unexpected events from the wrapper creation or the reparenting.
    m_frame.selectInput(frameEventMask());
    m_wrapper.selectInput(wrapperEventMask());
    m_client.selectInput(clientEventMask());

    updateMouseGrab();
}

void X11Window::updateInputWindow()
{
    if (!Xcb::Extensions::self()->isShapeInputAvailable()) {
        return;
    }

    if (kwinApp()->operationMode() != Application::OperationModeX11) {
        return;
    }

    QRegion region;

    if (decoration()) {
        const QMargins &r = decoration()->resizeOnlyBorders();
        const int left = r.left();
        const int top = r.top();
        const int right = r.right();
        const int bottom = r.bottom();
        if (left != 0 || top != 0 || right != 0 || bottom != 0) {
            region = QRegion(-left,
                             -top,
                             decoration()->size().width() + left + right,
                             decoration()->size().height() + top + bottom);
            region = region.subtracted(decoration()->rect());
        }
    }

    if (region.isEmpty()) {
        m_decoInputExtent.reset();
        return;
    }

    QRectF bounds = region.boundingRect();
    input_offset = bounds.topLeft();

    // Move the bounding rect to screen coordinates
    bounds.translate(frameGeometry().topLeft());

    // Move the region to input window coordinates
    region.translate(-input_offset.toPoint());

    if (!m_decoInputExtent.isValid()) {
        const uint32_t mask = XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK;
        const uint32_t values[] = {true,
                                   XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_LEAVE_WINDOW | XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_POINTER_MOTION};
        m_decoInputExtent.create(bounds, XCB_WINDOW_CLASS_INPUT_ONLY, mask, values);
        if (mapping_state == Mapped) {
            m_decoInputExtent.map();
        }
    } else {
        m_decoInputExtent.setGeometry(bounds);
    }

    const QList<xcb_rectangle_t> rects = Xcb::regionToRects(region);
    xcb_shape_rectangles(kwinApp()->x11Connection(), XCB_SHAPE_SO_SET, XCB_SHAPE_SK_INPUT, XCB_CLIP_ORDERING_UNSORTED,
                         m_decoInputExtent, 0, 0, rects.count(), rects.constData());
}

void X11Window::updateDecoration(bool check_workspace_pos, bool force)
{
    if (!force && ((!isDecorated() && noBorder()) || (isDecorated() && !noBorder()))) {
        return;
    }
    QRectF oldgeom = moveResizeGeometry();
    blockGeometryUpdates(true);
    if (force) {
        destroyDecoration();
    }
    if (!noBorder()) {
        createDecoration();
    } else {
        destroyDecoration();
    }
    updateShadow();
    if (check_workspace_pos) {
        checkWorkspacePosition(oldgeom);
    }
    updateInputWindow();
    blockGeometryUpdates(false);
    updateFrameExtents();
}

void X11Window::invalidateDecoration()
{
    updateDecoration(true, true);
}

void X11Window::createDecoration()
{
    std::shared_ptr<KDecoration2::Decoration> decoration(Workspace::self()->decorationBridge()->createDecoration(this));
    if (decoration) {
        connect(decoration.get(), &KDecoration2::Decoration::resizeOnlyBordersChanged, this, [this]() {
            if (!isDeleted()) {
                updateInputWindow();
            }
        });
        connect(decoration.get(), &KDecoration2::Decoration::bordersChanged, this, [this]() {
            if (!isDeleted()) {
                updateFrameExtents();
            }
        });
        connect(decoration.get(), &KDecoration2::Decoration::bordersChanged, this, [this]() {
            if (isDeleted()) {
                return;
            }
            const QRectF oldGeometry = moveResizeGeometry();
            if (!isShade()) {
                checkWorkspacePosition(oldGeometry);
            }
        });
        connect(decoratedClient()->decoratedClient(), &KDecoration2::DecoratedClient::sizeChanged, this, [this]() {
            if (!isDeleted()) {
                updateInputWindow();
            }
        });
    }
    setDecoration(decoration);

    moveResize(QRectF(calculateGravitation(false), clientSizeToFrameSize(clientSize())));
    maybeCreateX11DecorationRenderer();
}

void X11Window::destroyDecoration()
{
    if (isDecorated()) {
        QPointF grav = calculateGravitation(true);
        setDecoration(nullptr);
        maybeDestroyX11DecorationRenderer();
        moveResize(QRectF(grav, clientSizeToFrameSize(clientSize())));
    }
    m_decoInputExtent.reset();
}

void X11Window::maybeCreateX11DecorationRenderer()
{
    if (kwinApp()->operationMode() != Application::OperationModeX11) {
        return;
    }
    if (!Compositor::compositing() && decoratedClient()) {
        m_decorationRenderer = std::make_unique<X11DecorationRenderer>(decoratedClient());
        decoration()->update();
    }
}

void X11Window::maybeDestroyX11DecorationRenderer()
{
    m_decorationRenderer.reset();
}

void X11Window::detectNoBorder()
{
    if (is_shape) {
        noborder = true;
        app_noborder = true;
        return;
    }
    switch (windowType()) {
    case WindowType::Desktop:
    case WindowType::Dock:
    case WindowType::TopMenu:
    case WindowType::Splash:
    case WindowType::Notification:
    case WindowType::OnScreenDisplay:
    case WindowType::CriticalNotification:
    case WindowType::AppletPopup:
        noborder = true;
        app_noborder = true;
        break;
    case WindowType::Unknown:
    case WindowType::Normal:
    case WindowType::Toolbar:
    case WindowType::Menu:
    case WindowType::Dialog:
    case WindowType::Utility:
        noborder = false;
        break;
    default:
        Q_UNREACHABLE();
    }
    // WindowType::Override is some strange beast without clear definition, usually
    // just meaning "noborder", so let's treat it only as such flag, and ignore it as
    // a window type otherwise (SUPPORTED_WINDOW_TYPES_MASK doesn't include it)
    if (WindowType(info->windowType(NET::OverrideMask)) == WindowType::Override) {
        noborder = true;
        app_noborder = true;
    }
}

void X11Window::updateFrameExtents()
{
    NETStrut strut;
    strut.left = Xcb::toXNative(borderLeft());
    strut.right = Xcb::toXNative(borderRight());
    strut.top = Xcb::toXNative(borderTop());
    strut.bottom = Xcb::toXNative(borderBottom());
    info->setFrameExtents(strut);
}

void X11Window::setClientFrameExtents(const NETStrut &strut)
{
    const QMarginsF clientFrameExtents(Xcb::fromXNative(strut.left),
                                       Xcb::fromXNative(strut.top),
                                       Xcb::fromXNative(strut.right),
                                       Xcb::fromXNative(strut.bottom));
    if (m_clientFrameExtents == clientFrameExtents) {
        return;
    }

    m_clientFrameExtents = clientFrameExtents;

    // We should resize the client when its custom frame extents are changed so
    // the logical bounds remain the same. This however means that we will send
    // several configure requests to the application upon restoring it from the
    // maximized or fullscreen state. Notice that a client-side decorated client
    // cannot be shaded, therefore it's okay not to use the adjusted size here.
    moveResize(moveResizeGeometry());
}

/**
 * Resizes the decoration, and makes sure the decoration widget gets resize event
 * even if the size hasn't changed. This is needed to make sure the decoration
 * re-layouts (e.g. when maximization state changes,
 * the decoration may alter some borders, but the actual size
 * of the decoration stays the same).
 */
void X11Window::resizeDecoration()
{
    triggerDecorationRepaint();
    updateInputWindow();
}

bool X11Window::userNoBorder() const
{
    return noborder;
}

bool X11Window::isFullScreenable() const
{
    if (isUnmanaged()) {
        return false;
    }
    if (!rules()->checkFullScreen(true)) {
        return false;
    }
    // don't check size constrains - some apps request fullscreen despite requesting fixed size
    return isNormalWindow() || isDialog(); // also better disallow only weird types to go fullscreen
}

bool X11Window::noBorder() const
{
    return userNoBorder() || isFullScreen();
}

bool X11Window::userCanSetNoBorder() const
{
    if (isUnmanaged()) {
        return false;
    }

    // Client-side decorations and server-side decorations are mutually exclusive.
    if (isClientSideDecorated()) {
        return false;
    }

    return !isFullScreen() && !isShade();
}

void X11Window::setNoBorder(bool set)
{
    if (!userCanSetNoBorder()) {
        return;
    }
    set = rules()->checkNoBorder(set);
    if (noborder == set) {
        return;
    }
    noborder = set;
    updateDecoration(true, false);
    updateWindowRules(Rules::NoBorder);
}

void X11Window::checkNoBorder()
{
    setNoBorder(app_noborder);
}

void X11Window::detectShape()
{
    is_shape = Xcb::Extensions::self()->hasShape(window());
}

void X11Window::updateShape()
{
    if (is_shape) {
        // Workaround for #19644 - Shaped windows shouldn't have decoration
        if (!app_noborder) {
            // Only when shape is detected for the first time, still let the user to override
            app_noborder = true;
            noborder = rules()->checkNoBorder(true);
            updateDecoration(true);
        }
        if (!isDecorated()) {
            xcb_shape_combine(kwinApp()->x11Connection(),
                              XCB_SHAPE_SO_SET,
                              XCB_SHAPE_SK_BOUNDING,
                              XCB_SHAPE_SK_BOUNDING,
                              frameId(),
                              Xcb::toXNative(wrapperPos().x()),
                              Xcb::toXNative(wrapperPos().y()),
                              window());
        }
    } else if (app_noborder) {
        xcb_shape_mask(kwinApp()->x11Connection(), XCB_SHAPE_SO_SET, XCB_SHAPE_SK_BOUNDING, frameId(), 0, 0, XCB_PIXMAP_NONE);
        detectNoBorder();
        app_noborder = noborder;
        noborder = rules()->checkNoBorder(noborder || m_motif.noBorder());
        updateDecoration(true);
    }

    // Decoration mask (i.e. 'else' here) setting is done in setMask()
    // when the decoration calls it or when the decoration is created/destroyed
    updateInputShape();
    Q_EMIT shapeChanged();
}

static Xcb::Window shape_helper_window(XCB_WINDOW_NONE);

void X11Window::cleanupX11()
{
    shape_helper_window.reset();
}

void X11Window::updateInputShape()
{
    if (hiddenPreview()) { // Sets it to none, don't change
        return;
    }
    if (Xcb::Extensions::self()->isShapeInputAvailable()) {
        // There appears to be no way to find out if a window has input
        // shape set or not, so always propagate the input shape
        // (it's the same like the bounding shape by default).
        // Also, build the shape using a helper window, not directly
        // in the frame window, because the sequence set-shape-to-frame,
        // remove-shape-of-client, add-input-shape-of-client has the problem
        // that after the second step there's a hole in the input shape
        // until the real shape of the client is added and that can make
        // the window lose focus (which is a problem with mouse focus policies)
        // TODO: It seems there is, after all - XShapeGetRectangles() - but maybe this is better
        if (!shape_helper_window.isValid()) {
            shape_helper_window.create(QRect(0, 0, 1, 1));
        }
        const QSizeF bufferSize = m_bufferGeometry.size();
        shape_helper_window.resize(bufferSize);
        xcb_connection_t *c = kwinApp()->x11Connection();
        xcb_shape_combine(c, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_INPUT, XCB_SHAPE_SK_BOUNDING,
                          shape_helper_window, 0, 0, frameId());
        xcb_shape_combine(c,
                          XCB_SHAPE_SO_SUBTRACT,
                          XCB_SHAPE_SK_INPUT,
                          XCB_SHAPE_SK_BOUNDING,
                          shape_helper_window,
                          Xcb::toXNative(wrapperPos().x()),
                          Xcb::toXNative(wrapperPos().y()),
                          window());
        xcb_shape_combine(c,
                          XCB_SHAPE_SO_UNION,
                          XCB_SHAPE_SK_INPUT,
                          XCB_SHAPE_SK_INPUT,
                          shape_helper_window,
                          Xcb::toXNative(wrapperPos().x()),
                          Xcb::toXNative(wrapperPos().y()),
                          window());
        xcb_shape_combine(c, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_INPUT, XCB_SHAPE_SK_INPUT,
                          frameId(), 0, 0, shape_helper_window);
    }
}

bool X11Window::setupCompositing()
{
    if (!Window::setupCompositing()) {
        return false;
    }
    // If compositing is back on, stop rendering decoration in the frame window.
    maybeDestroyX11DecorationRenderer();
    updateVisibility(); // for internalKeep()
    return true;
}

void X11Window::finishCompositing()
{
    Window::finishCompositing();
    updateVisibility();
    // If compositing is off, render the decoration in the X11 frame window.
    maybeCreateX11DecorationRenderer();
}

/**
 * Returns whether the window is minimizable or not
 */
bool X11Window::isMinimizable() const
{
    if (isSpecialWindow() && !isTransient()) {
        return false;
    }
    if (isAppletPopup()) {
        return false;
    }
    if (!rules()->checkMinimize(true)) {
        return false;
    }

    if (isTransient()) {
        // #66868 - Let other xmms windows be minimized when the mainwindow is minimized
        bool shown_mainwindow = false;
        auto mainwindows = mainWindows();
        for (auto it = mainwindows.constBegin(); it != mainwindows.constEnd(); ++it) {
            if ((*it)->isShown()) {
                shown_mainwindow = true;
            }
        }
        if (!shown_mainwindow) {
            return true;
        }
    }
#if 0
    // This is here because kicker's taskbar doesn't provide separate entries
    // for windows with an explicitly given parent
    // TODO: perhaps this should be redone
    // Disabled for now, since at least modal dialogs should be minimizable
    // (resulting in the mainwindow being minimized too).
    if (transientFor() != NULL)
        return false;
#endif
    if (!wantsTabFocus()) { // SELI, TODO: - NET::Utility? why wantsTabFocus() - skiptaskbar? ?
        return false;
    }
    return true;
}

void X11Window::doMinimize()
{
    if (m_managed) {
        if (isMinimized()) {
            workspace()->activateNextWindow(this);
        }
    }
    if (isShade()) {
        // NETWM restriction - KWindowInfo::isMinimized() == Hidden && !Shaded
        info->setState(isMinimized() ? NET::States() : NET::Shaded, NET::Shaded);
    }
    updateVisibility();
    updateAllowedActions();
    workspace()->updateMinimizedOfTransients(this);
}

QRectF X11Window::iconGeometry() const
{
    NETRect r = info->iconGeometry();
    QRectF geom = Xcb::fromXNative(QRect(r.pos.x, r.pos.y, r.size.width, r.size.height));
    if (geom.isValid()) {
        return geom;
    } else {
        // Check all mainwindows of this window (recursively)
        const auto &clients = mainWindows();
        for (Window *amainwin : clients) {
            X11Window *mainwin = dynamic_cast<X11Window *>(amainwin);
            if (!mainwin) {
                continue;
            }
            geom = mainwin->iconGeometry();
            if (geom.isValid()) {
                return geom;
            }
        }
        // No mainwindow (or their parents) with icon geometry was found
        return Window::iconGeometry();
    }
}

bool X11Window::isShadeable() const
{
    return !isSpecialWindow() && isDecorated() && (rules()->checkShade(ShadeNormal) != rules()->checkShade(ShadeNone));
}

void X11Window::doSetShade(ShadeMode previousShadeMode)
{
    // TODO: All this unmapping, resizing etc. feels too much duplicated from elsewhere
    if (isShade()) {
        shade_geometry_change = true;
        QSizeF s(implicitSize());
        s.setHeight(borderTop() + borderBottom());
        m_wrapper.selectInput(wrapperEventMask() & ~XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY); // Avoid getting UnmapNotify
        m_wrapper.unmap();
        m_client.unmap();
        m_wrapper.selectInput(wrapperEventMask());
        exportMappingState(XCB_ICCCM_WM_STATE_ICONIC);
        resize(s);
        shade_geometry_change = false;
        if (previousShadeMode == ShadeHover) {
            if (shade_below && workspace()->stackingOrder().indexOf(shade_below) > -1) {
                workspace()->restack(this, shade_below, true);
            }
            if (isActive()) {
                workspace()->activateNextWindow(this);
            }
        } else if (isActive()) {
            workspace()->focusToNull();
        }
    } else {
        shade_geometry_change = true;
        if (decoratedClient()) {
            decoratedClient()->signalShadeChange();
        }
        QSizeF s(implicitSize());
        shade_geometry_change = false;
        resize(s);
        setGeometryRestore(moveResizeGeometry());
        if ((shadeMode() == ShadeHover || shadeMode() == ShadeActivated) && rules()->checkAcceptFocus(info->input())) {
            setActive(true);
        }
        if (shadeMode() == ShadeHover) {
            QList<Window *> order = workspace()->stackingOrder();
            // invalidate, since "this" could be the topmost toplevel and shade_below dangeling
            shade_below = nullptr;
            // this is likely related to the index parameter?!
            for (int idx = order.indexOf(this) + 1; idx < order.count(); ++idx) {
                shade_below = qobject_cast<X11Window *>(order.at(idx));
                if (shade_below) {
                    break;
                }
            }
            if (shade_below && shade_below->isNormalWindow()) {
                workspace()->raiseWindow(this);
            } else {
                shade_below = nullptr;
            }
        }
        m_wrapper.map();
        m_client.map();
        exportMappingState(XCB_ICCCM_WM_STATE_NORMAL);
        if (isActive()) {
            workspace()->requestFocus(this);
        }
    }
    info->setState(isShade() ? NET::Shaded : NET::States(), NET::Shaded);
    info->setState((isShade() || !isShown()) ? NET::Hidden : NET::States(), NET::Hidden);
    updateVisibility();
    updateAllowedActions();
    discardWindowPixmap();
}

void X11Window::updateVisibility()
{
    if (isUnmanaged() || isDeleted()) {
        return;
    }
    if (isHidden()) {
        info->setState(NET::Hidden, NET::Hidden);
        setSkipTaskbar(true); // Also hide from taskbar
        if (Compositor::compositing() && options->hiddenPreviews() == HiddenPreviewsAlways) {
            internalKeep();
        } else {
            internalHide();
        }
        return;
    }
    if (isHiddenByShowDesktop()) {
        if (waylandServer()) {
            return;
        }
        if (Compositor::compositing() && options->hiddenPreviews() != HiddenPreviewsNever) {
            internalKeep();
        } else {
            internalHide();
        }
        return;
    }
    setSkipTaskbar(originalSkipTaskbar()); // Reset from 'hidden'
    if (isMinimized()) {
        info->setState(NET::Hidden, NET::Hidden);
        if (Compositor::compositing() && options->hiddenPreviews() == HiddenPreviewsAlways) {
            internalKeep();
        } else {
            internalHide();
        }
        return;
    }
    info->setState(NET::States(), NET::Hidden);
    if (!isOnCurrentDesktop()) {
        if (Compositor::compositing() && options->hiddenPreviews() != HiddenPreviewsNever) {
            internalKeep();
        } else {
            internalHide();
        }
        return;
    }
    if (!isOnCurrentActivity()) {
        if (Compositor::compositing() && options->hiddenPreviews() != HiddenPreviewsNever) {
            internalKeep();
        } else {
            internalHide();
        }
        return;
    }
    internalShow();
}

/**
 * Sets the client window's mapping state. Possible values are
 * WithdrawnState, IconicState, NormalState.
 */
void X11Window::exportMappingState(int s)
{
    Q_ASSERT(m_client != XCB_WINDOW_NONE);
    Q_ASSERT(!isDeleted() || s == XCB_ICCCM_WM_STATE_WITHDRAWN);
    if (s == XCB_ICCCM_WM_STATE_WITHDRAWN) {
        m_client.deleteProperty(atoms->wm_state);
        return;
    }
    Q_ASSERT(s == XCB_ICCCM_WM_STATE_NORMAL || s == XCB_ICCCM_WM_STATE_ICONIC);

    int32_t data[2];
    data[0] = s;
    data[1] = XCB_NONE;
    m_client.changeProperty(atoms->wm_state, atoms->wm_state, 32, 2, data);
}

void X11Window::internalShow()
{
    if (mapping_state == Mapped) {
        return;
    }
    MappingState old = mapping_state;
    mapping_state = Mapped;
    if (old == Unmapped || old == Withdrawn) {
        map();
    }
    if (old == Kept) {
        m_decoInputExtent.map();
        updateHiddenPreview();
    }
}

void X11Window::internalHide()
{
    if (mapping_state == Unmapped) {
        return;
    }
    MappingState old = mapping_state;
    mapping_state = Unmapped;
    if (old == Mapped || old == Kept) {
        unmap();
    }
    if (old == Kept) {
        updateHiddenPreview();
    }
}

void X11Window::internalKeep()
{
    Q_ASSERT(Compositor::compositing());
    if (mapping_state == Kept) {
        return;
    }
    MappingState old = mapping_state;
    mapping_state = Kept;
    if (old == Unmapped || old == Withdrawn) {
        map();
    }
    m_decoInputExtent.unmap();
    if (isActive()) {
        workspace()->focusToNull(); // get rid of input focus, bug #317484
    }
    updateHiddenPreview();
}

/**
 * Maps (shows) the client. Note that it is mapping state of the frame,
 * not necessarily the client window itself (i.e. a shaded window is here
 * considered mapped, even though it is in IconicState).
 */
void X11Window::map()
{
    // XComposite invalidates backing pixmaps on unmap (minimize, different
    // virtual desktop, etc.).  We kept the last known good pixmap around
    // for use in effects, but now we want to have access to the new pixmap
    if (Compositor::compositing()) {
        discardWindowPixmap();
    }
    m_frame.map();
    if (!isShade()) {
        m_wrapper.map();
        m_client.map();
        m_decoInputExtent.map();
        exportMappingState(XCB_ICCCM_WM_STATE_NORMAL);
    } else {
        exportMappingState(XCB_ICCCM_WM_STATE_ICONIC);
    }
}

/**
 * Unmaps the client. Again, this is about the frame.
 */
void X11Window::unmap()
{
    // Here it may look like a race condition, as some other client might try to unmap
    // the window between these two XSelectInput() calls. However, they're supposed to
    // use XWithdrawWindow(), which also sends a synthetic event to the root window,
    // which won't be missed, so this shouldn't be a problem. The chance the real UnmapNotify
    // will be missed is also very minimal, so I don't think it's needed to grab the server
    // here.
    m_wrapper.selectInput(wrapperEventMask() & ~XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY); // Avoid getting UnmapNotify
    m_frame.unmap();
    m_wrapper.unmap();
    m_client.unmap();
    m_decoInputExtent.unmap();
    m_wrapper.selectInput(wrapperEventMask());
    exportMappingState(XCB_ICCCM_WM_STATE_ICONIC);
}

/**
 * XComposite doesn't keep window pixmaps of unmapped windows, which means
 * there wouldn't be any previews of windows that are minimized or on another
 * virtual desktop. Therefore rawHide() actually keeps such windows mapped.
 * However special care needs to be taken so that such windows don't interfere.
 * Therefore they're put very low in the stacking order and they have input shape
 * set to none, which hopefully is enough. If there's no input shape available,
 * then it's hoped that there will be some other desktop above it *shrug*.
 * Using normal shape would be better, but that'd affect other things, e.g. painting
 * of the actual preview.
 */
void X11Window::updateHiddenPreview()
{
    if (hiddenPreview()) {
        workspace()->forceRestacking();
        if (Xcb::Extensions::self()->isShapeInputAvailable()) {
            xcb_shape_rectangles(kwinApp()->x11Connection(), XCB_SHAPE_SO_SET, XCB_SHAPE_SK_INPUT,
                                 XCB_CLIP_ORDERING_UNSORTED, frameId(), 0, 0, 0, nullptr);
        }
    } else {
        workspace()->forceRestacking();
        updateInputShape();
    }
}

void X11Window::sendClientMessage(xcb_window_t w, xcb_atom_t a, xcb_atom_t protocol, uint32_t data1, uint32_t data2, uint32_t data3)
{
    xcb_client_message_event_t ev;
    // Every X11 event is 32 bytes (see man xcb_send_event), so XCB will copy
    // 32 unconditionally. Add a static_assert to ensure we don't disclose
    // stack memory.
    static_assert(sizeof(ev) == 32, "Would leak stack data otherwise");
    memset(&ev, 0, sizeof(ev));
    ev.response_type = XCB_CLIENT_MESSAGE;
    ev.window = w;
    ev.type = a;
    ev.format = 32;
    ev.data.data32[0] = protocol;
    ev.data.data32[1] = xTime();
    ev.data.data32[2] = data1;
    ev.data.data32[3] = data2;
    ev.data.data32[4] = data3;
    uint32_t eventMask = 0;
    if (w == kwinApp()->x11RootWindow()) {
        eventMask = XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT; // Magic!
    }
    xcb_send_event(kwinApp()->x11Connection(), false, w, eventMask, reinterpret_cast<const char *>(&ev));
    xcb_flush(kwinApp()->x11Connection());
}

/**
 * Returns whether the window may be closed (have a close button)
 */
bool X11Window::isCloseable() const
{
    return !isUnmanaged() && rules()->checkCloseable(m_motif.close() && !isSpecialWindow());
}

/**
 * Closes the window by either sending a delete_window message or using XKill.
 */
void X11Window::closeWindow()
{
    if (!isCloseable()) {
        return;
    }

    // Update user time, because the window may create a confirming dialog.
    updateUserTime();

    if (info->supportsProtocol(NET::DeleteWindowProtocol)) {
        sendClientMessage(window(), atoms->wm_protocols, atoms->wm_delete_window);
        pingWindow();
    } else { // Client will not react on wm_delete_window. We have not choice
        // but destroy his connection to the XServer.
        killWindow();
    }
}

/**
 * Kills the window via XKill
 */
void X11Window::killWindow()
{
    qCDebug(KWIN_CORE) << "X11Window::killWindow():" << window();
    if (isUnmanaged()) {
        xcb_kill_client(kwinApp()->x11Connection(), window());
    } else {
        killProcess(false);
        m_client.kill(); // Always kill this client at the server
        destroyWindow();
    }
}

/**
 * Send a ping to the window using _NET_WM_PING if possible if it
 * doesn't respond within a reasonable time, it will be killed.
 */
void X11Window::pingWindow()
{
    if (!info->supportsProtocol(NET::PingProtocol)) {
        return; // Can't ping :(
    }
    if (options->killPingTimeout() == 0) {
        return; // Turned off
    }
    if (ping_timer != nullptr) {
        return; // Pinging already
    }
    ping_timer = new QTimer(this);
    connect(ping_timer, &QTimer::timeout, this, [this]() {
        if (unresponsive()) {
            qCDebug(KWIN_CORE) << "Final ping timeout, asking to kill:" << caption();
            ping_timer->deleteLater();
            ping_timer = nullptr;
            killProcess(true, m_pingTimestamp);
            return;
        }

        qCDebug(KWIN_CORE) << "First ping timeout:" << caption();

        setUnresponsive(true);
        ping_timer->start();
    });
    ping_timer->setSingleShot(true);
    // we'll run the timer twice, at first we'll desaturate the window
    // and the second time we'll show the "do you want to kill" prompt
    ping_timer->start(options->killPingTimeout() / 2);
    m_pingTimestamp = xTime();
    rootInfo()->sendPing(window(), m_pingTimestamp);
}

void X11Window::gotPing(xcb_timestamp_t timestamp)
{
    // Just plain compare is not good enough because of 64bit and truncating and whatnot
    if (NET::timestampCompare(timestamp, m_pingTimestamp) != 0) {
        return;
    }
    delete ping_timer;
    ping_timer = nullptr;

    setUnresponsive(false);

    if (m_killPrompt) {
        m_killPrompt->quit();
    }
}

void X11Window::killProcess(bool ask, xcb_timestamp_t timestamp)
{
    if (m_killPrompt && m_killPrompt->isRunning()) {
        return;
    }
    Q_ASSERT(!ask || timestamp != XCB_TIME_CURRENT_TIME);
    pid_t pid = info->pid();
    if (pid <= 0 || clientMachine()->hostName().isEmpty()) { // Needed properties missing
        return;
    }
    qCDebug(KWIN_CORE) << "Kill process:" << pid << "(" << clientMachine()->hostName() << ")";
    if (!ask) {
        if (!clientMachine()->isLocal()) {
            QStringList lst;
            lst << clientMachine()->hostName() << QStringLiteral("kill") << QString::number(pid);
            QProcess::startDetached(QStringLiteral("xon"), lst);
        } else {
            ::kill(pid, SIGTERM);
        }
    } else {
        if (!m_killPrompt) {
            m_killPrompt = std::make_unique<KillPrompt>(this);
        }
        m_killPrompt->start(timestamp);
    }
}

void X11Window::doSetKeepAbove()
{
    info->setState(keepAbove() ? NET::KeepAbove : NET::States(), NET::KeepAbove);
}

void X11Window::doSetKeepBelow()
{
    info->setState(keepBelow() ? NET::KeepBelow : NET::States(), NET::KeepBelow);
}

void X11Window::doSetSkipTaskbar()
{
    info->setState(skipTaskbar() ? NET::SkipTaskbar : NET::States(), NET::SkipTaskbar);
}

void X11Window::doSetSkipPager()
{
    info->setState(skipPager() ? NET::SkipPager : NET::States(), NET::SkipPager);
}

void X11Window::doSetSkipSwitcher()
{
    info->setState(skipSwitcher() ? NET::SkipSwitcher : NET::States(), NET::SkipSwitcher);
}

void X11Window::doSetDesktop()
{
    info->setDesktop(desktopId());
    updateVisibility();
}

void X11Window::doSetDemandsAttention()
{
    info->setState(isDemandingAttention() ? NET::DemandsAttention : NET::States(), NET::DemandsAttention);
}

void X11Window::doSetHidden()
{
    updateVisibility();
}

void X11Window::doSetHiddenByShowDesktop()
{
    updateVisibility();
}

void X11Window::doSetModal()
{
    info->setState(isModal() ? NET::Modal : NET::States(), NET::Modal);
}

void X11Window::doSetOnActivities(const QStringList &activityList)
{
#if KWIN_BUILD_ACTIVITIES
    if (activityList.isEmpty()) {
        const QByteArray nullUuid = Activities::nullUuid().toUtf8();
        m_client.changeProperty(atoms->activities, XCB_ATOM_STRING, 8, nullUuid.length(), nullUuid.constData());
    } else {
        QByteArray joined = activityList.join(QStringLiteral(",")).toLatin1();
        m_client.changeProperty(atoms->activities, XCB_ATOM_STRING, 8, joined.length(), joined.constData());
    }
#endif
}

void X11Window::updateActivities(bool includeTransients)
{
    Window::updateActivities(includeTransients);
    if (!m_activityUpdatesBlocked) {
        updateVisibility();
    }
}

/**
 * Returns the list of activities the client window is on.
 * if it's on all activities, the list will be empty.
 * Don't use this, use isOnActivity() and friends (from class Window)
 */
QStringList X11Window::activities() const
{
    if (sessionActivityOverride) {
        return QStringList();
    }
    return Window::activities();
}

/**
 * Performs the actual focusing of the window using XSetInputFocus and WM_TAKE_FOCUS
 */
bool X11Window::takeFocus()
{
    const bool effectiveAcceptFocus = rules()->checkAcceptFocus(info->input());
    const bool effectiveTakeFocus = rules()->checkAcceptFocus(info->supportsProtocol(NET::TakeFocusProtocol));

    if (effectiveAcceptFocus) {
        xcb_void_cookie_t cookie = xcb_set_input_focus_checked(kwinApp()->x11Connection(),
                                                               XCB_INPUT_FOCUS_POINTER_ROOT,
                                                               window(), XCB_TIME_CURRENT_TIME);
        UniqueCPtr<xcb_generic_error_t> error(xcb_request_check(kwinApp()->x11Connection(), cookie));
        if (error) {
            qCWarning(KWIN_CORE, "Failed to focus 0x%x (error %d)", window(), error->error_code);
            return false;
        }
    } else {
        demandAttention(false); // window cannot take input, at least withdraw urgency
    }
    if (effectiveTakeFocus) {
        kwinApp()->updateXTime();
        sendClientMessage(window(), atoms->wm_protocols, atoms->wm_take_focus);
    }

    if (effectiveAcceptFocus || effectiveTakeFocus) {
        workspace()->setShouldGetFocus(this);
    }
    return true;
}

/**
 * Returns whether the window provides context help or not. If it does,
 * you should show a help menu item or a help button like '?' and call
 * contextHelp() if this is invoked.
 *
 * \sa contextHelp()
 */
bool X11Window::providesContextHelp() const
{
    return info->supportsProtocol(NET::ContextHelpProtocol);
}

/**
 * Invokes context help on the window. Only works if the window
 * actually provides context help.
 *
 * \sa providesContextHelp()
 */
void X11Window::showContextHelp()
{
    if (info->supportsProtocol(NET::ContextHelpProtocol)) {
        sendClientMessage(window(), atoms->wm_protocols, atoms->net_wm_context_help);
    }
}

/**
 * Fetches the window's caption (WM_NAME property). It will be
 * stored in the client's caption().
 */
void X11Window::fetchName()
{
    setCaption(readName());
}

static inline QString readNameProperty(xcb_window_t w, xcb_atom_t atom)
{
    const auto cookie = xcb_icccm_get_text_property_unchecked(kwinApp()->x11Connection(), w, atom);
    xcb_icccm_get_text_property_reply_t reply;
    if (xcb_icccm_get_wm_name_reply(kwinApp()->x11Connection(), cookie, &reply, nullptr)) {
        QString retVal;
        if (reply.encoding == atoms->utf8_string) {
            retVal = QString::fromUtf8(QByteArray(reply.name, reply.name_len));
        } else if (reply.encoding == XCB_ATOM_STRING) {
            retVal = QString::fromLatin1(QByteArray(reply.name, reply.name_len));
        }
        xcb_icccm_get_text_property_reply_wipe(&reply);
        return retVal.simplified();
    }
    return QString();
}

QString X11Window::readName() const
{
    if (info->name() && info->name()[0] != '\0') {
        return QString::fromUtf8(info->name()).simplified();
    } else {
        return readNameProperty(window(), XCB_ATOM_WM_NAME);
    }
}

// The list is taken from https://www.unicode.org/reports/tr9/ (#154840)
static const QChar LRM(0x200E);

void X11Window::setCaption(const QString &_s, bool force)
{
    QString s(_s);
    for (int i = 0; i < s.length();) {
        if (!s[i].isPrint()) {
            if (QChar(s[i]).isHighSurrogate() && i + 1 < s.length() && QChar(s[i + 1]).isLowSurrogate()) {
                const uint uc = QChar::surrogateToUcs4(s[i], s[i + 1]);
                if (!QChar::isPrint(uc)) {
                    s.remove(i, 2);
                } else {
                    i += 2;
                }
                continue;
            }
            s.remove(i, 1);
            continue;
        }
        ++i;
    }
    const bool changed = (s != cap_normal);
    if (!force && !changed) {
        return;
    }
    cap_normal = s;

    bool was_suffix = (!cap_suffix.isEmpty());
    cap_suffix.clear();
    QString machine_suffix;
    if (!options->condensedTitle()) { // machine doesn't qualify for "clean"
        if (clientMachine()->hostName() != ClientMachine::localhost() && !clientMachine()->isLocal()) {
            machine_suffix = QLatin1String(" <@") + clientMachine()->hostName() + QLatin1Char('>') + LRM;
        }
    }
    QString shortcut_suffix = shortcutCaptionSuffix();
    cap_suffix = machine_suffix + shortcut_suffix;
    if ((was_suffix && cap_suffix.isEmpty()) || force) {
        // If it was new window, it may have old value still set, if the window is reused
        info->setVisibleName("");
        info->setVisibleIconName("");
    } else if (!cap_suffix.isEmpty() && !cap_iconic.isEmpty()) {
        // Keep the same suffix in iconic name if it's set
        info->setVisibleIconName(QString(cap_iconic + cap_suffix).toUtf8().constData());
    }

    if (changed) {
        Q_EMIT captionNormalChanged();
    }
    Q_EMIT captionChanged();
}

void X11Window::updateCaption()
{
    setCaption(cap_normal, true);
}

void X11Window::fetchIconicName()
{
    QString s;
    if (info->iconName() && info->iconName()[0] != '\0') {
        s = QString::fromUtf8(info->iconName());
    } else {
        s = readNameProperty(window(), XCB_ATOM_WM_ICON_NAME);
    }
    if (s != cap_iconic) {
        bool was_set = !cap_iconic.isEmpty();
        cap_iconic = s;
        if (!cap_suffix.isEmpty()) {
            if (!cap_iconic.isEmpty()) { // Keep the same suffix in iconic name if it's set
                info->setVisibleIconName(QString(s + cap_suffix).toUtf8().constData());
            } else if (was_set) {
                info->setVisibleIconName("");
            }
        }
    }
}

void X11Window::getMotifHints()
{
    const bool wasClosable = isCloseable();
    const bool wasNoBorder = m_motif.noBorder();
    if (m_managed) { // only on property change, initial read is prefetched
        m_motif.fetch();
    }
    m_motif.read();
    if (m_motif.hasDecoration() && m_motif.noBorder() != wasNoBorder) {
        // If we just got a hint telling us to hide decorations, we do so.
        if (m_motif.noBorder()) {
            noborder = rules()->checkNoBorder(true);
            // If the Motif hint is now telling us to show decorations, we only do so if the app didn't
            // instruct us to hide decorations in some other way, though.
        } else if (!app_noborder) {
            noborder = rules()->checkNoBorder(false);
        }
    }

    // mminimize; - Ignore, bogus - E.g. shading or sending to another desktop is "minimizing" too
    // mmaximize; - Ignore, bogus - Maximizing is basically just resizing
    const bool closabilityChanged = wasClosable != isCloseable();
    if (isManaged()) {
        updateDecoration(true); // Check if noborder state has changed
    }
    if (closabilityChanged) {
        Q_EMIT closeableChanged(isCloseable());
    }
}

void X11Window::getIcons()
{
    if (isUnmanaged()) {
        return;
    }
    // First read icons from the window itself
    const QString themedIconName = iconFromDesktopFile();
    if (!themedIconName.isEmpty()) {
        setIcon(QIcon::fromTheme(themedIconName));
        return;
    }
    QIcon icon;
    auto readIcon = [this, &icon](int size, bool scale = true) {
        const QPixmap pix = KX11Extras::icon(window(), size, size, scale, KX11Extras::NETWM | KX11Extras::WMHints, info);
        if (!pix.isNull()) {
            icon.addPixmap(pix);
        }
    };
    readIcon(16);
    readIcon(32);
    readIcon(48, false);
    readIcon(64, false);
    readIcon(128, false);
    if (icon.isNull()) {
        // Then try window group
        icon = group()->icon();
    }
    if (icon.isNull() && isTransient()) {
        // Then mainwindows
        auto mainwindows = mainWindows();
        for (auto it = mainwindows.constBegin(); it != mainwindows.constEnd() && icon.isNull(); ++it) {
            if (!(*it)->icon().isNull()) {
                icon = (*it)->icon();
                break;
            }
        }
    }
    if (icon.isNull()) {
        // And if nothing else, load icon from classhint or xapp icon
        icon.addPixmap(KX11Extras::icon(window(), 32, 32, true, KX11Extras::ClassHint | KX11Extras::XApp, info));
        icon.addPixmap(KX11Extras::icon(window(), 16, 16, true, KX11Extras::ClassHint | KX11Extras::XApp, info));
        icon.addPixmap(KX11Extras::icon(window(), 64, 64, false, KX11Extras::ClassHint | KX11Extras::XApp, info));
        icon.addPixmap(KX11Extras::icon(window(), 128, 128, false, KX11Extras::ClassHint | KX11Extras::XApp, info));
    }
    setIcon(icon);
}

/**
 * Returns \c true if X11Client wants to throttle resizes; otherwise returns \c false.
 */
bool X11Window::wantsSyncCounter() const
{
    if (!waylandServer()) {
        return true;
    }
    // When the frame window is resized, the attached buffer will be destroyed by
    // Xwayland, causing unexpected invalid previous and current window pixmaps.
    // With the addition of multiple window buffers in Xwayland 1.21, X11 clients
    // are no longer able to destroy the buffer after it's been committed and not
    // released by the compositor yet.
    static const quint32 xwaylandVersion = xcb_get_setup(kwinApp()->x11Connection())->release_number;
    return xwaylandVersion >= 12100000;
}

void X11Window::getSyncCounter()
{
    if (!Xcb::Extensions::self()->isSyncAvailable()) {
        return;
    }
    if (!wantsSyncCounter()) {
        return;
    }

    Xcb::Property syncProp(false, window(), atoms->net_wm_sync_request_counter, XCB_ATOM_CARDINAL, 0, 1);
    const xcb_sync_counter_t counter = syncProp.value<xcb_sync_counter_t>(XCB_NONE);
    if (counter != XCB_NONE) {
        m_syncRequest.counter = counter;
        m_syncRequest.value.hi = 0;
        m_syncRequest.value.lo = 0;
        auto *c = kwinApp()->x11Connection();
        xcb_sync_set_counter(c, m_syncRequest.counter, m_syncRequest.value);
        if (m_syncRequest.alarm == XCB_NONE) {
            const uint32_t mask = XCB_SYNC_CA_COUNTER | XCB_SYNC_CA_VALUE_TYPE | XCB_SYNC_CA_TEST_TYPE | XCB_SYNC_CA_EVENTS;
            const uint32_t values[] = {
                m_syncRequest.counter,
                XCB_SYNC_VALUETYPE_RELATIVE,
                XCB_SYNC_TESTTYPE_POSITIVE_TRANSITION,
                1};
            m_syncRequest.alarm = xcb_generate_id(c);
            auto cookie = xcb_sync_create_alarm_checked(c, m_syncRequest.alarm, mask, values);
            UniqueCPtr<xcb_generic_error_t> error(xcb_request_check(c, cookie));
            if (error) {
                m_syncRequest.alarm = XCB_NONE;
            } else {
                xcb_sync_change_alarm_value_list_t value;
                memset(&value, 0, sizeof(value));
                value.value.hi = 0;
                value.value.lo = 1;
                value.delta.hi = 0;
                value.delta.lo = 1;
                xcb_sync_change_alarm_aux(c, m_syncRequest.alarm, XCB_SYNC_CA_DELTA | XCB_SYNC_CA_VALUE, &value);
            }
        }
    }
}

/**
 * Send the client a _NET_SYNC_REQUEST
 */
void X11Window::sendSyncRequest()
{
    if (m_syncRequest.counter == XCB_NONE || m_syncRequest.isPending) {
        return; // do NOT, NEVER send a sync request when there's one on the stack. the clients will just stop respoding. FOREVER! ...
    }

    if (!m_syncRequest.failsafeTimeout) {
        m_syncRequest.failsafeTimeout = new QTimer(this);
        connect(m_syncRequest.failsafeTimeout, &QTimer::timeout, this, [this]() {
            // client does not respond to XSYNC requests in reasonable time, remove support
            if (!ready_for_painting) {
                // failed on initial pre-show request
                setReadyForPainting();
                return;
            }
            // failed during resize
            m_syncRequest.isPending = false;
            m_syncRequest.interactiveResize = false;
            m_syncRequest.counter = XCB_NONE;
            m_syncRequest.alarm = XCB_NONE;
            delete m_syncRequest.timeout;
            delete m_syncRequest.failsafeTimeout;
            m_syncRequest.timeout = nullptr;
            m_syncRequest.failsafeTimeout = nullptr;
            m_syncRequest.lastTimestamp = XCB_CURRENT_TIME;
        });
        m_syncRequest.failsafeTimeout->setSingleShot(true);
    }
    // if there's no response within 10 seconds, sth. went wrong and we remove XSYNC support from this client.
    // see events.cpp X11Window::syncEvent()
    m_syncRequest.failsafeTimeout->start(ready_for_painting ? 10000 : 1000);

    // We increment before the notify so that after the notify
    // syncCounterSerial will equal the value we are expecting
    // in the acknowledgement
    const uint32_t oldLo = m_syncRequest.value.lo;
    m_syncRequest.value.lo++;
    if (oldLo > m_syncRequest.value.lo) {
        m_syncRequest.value.hi++;
    }
    if (m_syncRequest.lastTimestamp >= xTime()) {
        kwinApp()->updateXTime();
    }

    // Send the message to client
    sendClientMessage(window(), atoms->wm_protocols, atoms->net_wm_sync_request,
                      m_syncRequest.value.lo, m_syncRequest.value.hi);
    m_syncRequest.isPending = true;
    m_syncRequest.interactiveResize = isInteractiveResize();
    m_syncRequest.lastTimestamp = xTime();
}

bool X11Window::wantsInput() const
{
    return rules()->checkAcceptFocus(acceptsFocus() || info->supportsProtocol(NET::TakeFocusProtocol));
}

bool X11Window::acceptsFocus() const
{
    return info->input();
}

void X11Window::doSetQuickTileMode()
{
    setTile(workspace()->tileManager(output())->quickTile(m_requestedQuickTileMode));
}

void X11Window::setBlockingCompositing(bool block)
{
    const bool blocks = rules()->checkBlockCompositing(block && options->windowsBlockCompositing());
    if (blocks) {
        blockCompositing();
    } else {
        unblockCompositing();
    }
}

void X11Window::blockCompositing()
{
    if (blocks_compositing) {
        return;
    }
    blocks_compositing = true;
    Compositor::self()->inhibit(this);
}

void X11Window::unblockCompositing()
{
    if (!blocks_compositing) {
        return;
    }
    blocks_compositing = false;
    Compositor::self()->uninhibit(this);
}

void X11Window::updateAllowedActions(bool force)
{
    if (!isManaged() && !force) {
        return;
    }
    NET::Actions old_allowed_actions = NET::Actions(allowed_actions);
    allowed_actions = NET::Actions();
    if (isMovable()) {
        allowed_actions |= NET::ActionMove;
    }
    if (isResizable()) {
        allowed_actions |= NET::ActionResize;
    }
    if (isMinimizable()) {
        allowed_actions |= NET::ActionMinimize;
    }
    if (isShadeable()) {
        allowed_actions |= NET::ActionShade;
    }
    // Sticky state not supported
    if (isMaximizable()) {
        allowed_actions |= NET::ActionMax;
    }
    if (isFullScreenable()) {
        allowed_actions |= NET::ActionFullScreen;
    }
    allowed_actions |= NET::ActionChangeDesktop; // Always (Pagers shouldn't show Docks etc.)
    if (isCloseable()) {
        allowed_actions |= NET::ActionClose;
    }
    if (old_allowed_actions == allowed_actions) {
        return;
    }
    // TODO: This could be delayed and compressed - It's only for pagers etc. anyway
    info->setAllowedActions(allowed_actions);
    // ONLY if relevant features have changed (and the window didn't just get/loose moveresize for maximization state changes)
    const NET::Actions relevant = ~(NET::ActionMove | NET::ActionResize);
    if ((allowed_actions & relevant) != (old_allowed_actions & relevant)) {
        if ((allowed_actions & NET::ActionMinimize) != (old_allowed_actions & NET::ActionMinimize)) {
            Q_EMIT minimizeableChanged(allowed_actions & NET::ActionMinimize);
        }
        if ((allowed_actions & NET::ActionShade) != (old_allowed_actions & NET::ActionShade)) {
            Q_EMIT shadeableChanged(allowed_actions & NET::ActionShade);
        }
        if ((allowed_actions & NET::ActionMax) != (old_allowed_actions & NET::ActionMax)) {
            Q_EMIT maximizeableChanged(allowed_actions & NET::ActionMax);
        }
        if ((allowed_actions & NET::ActionClose) != (old_allowed_actions & NET::ActionClose)) {
            Q_EMIT closeableChanged(allowed_actions & NET::ActionClose);
        }
    }
}

Xcb::StringProperty X11Window::fetchActivities() const
{
#if KWIN_BUILD_ACTIVITIES
    return Xcb::StringProperty(window(), atoms->activities);
#else
    return Xcb::StringProperty();
#endif
}

void X11Window::readActivities(Xcb::StringProperty &property)
{
#if KWIN_BUILD_ACTIVITIES
    QString prop = QString::fromUtf8(property);
    activitiesDefined = !prop.isEmpty();

    if (prop == Activities::nullUuid()) {
        // copied from setOnAllActivities to avoid a redundant XChangeProperty.
        if (!m_activityList.isEmpty()) {
            m_activityList.clear();
            updateActivities(true);
        }
        return;
    }
    if (prop.isEmpty()) {
        // note: this makes it *act* like it's on all activities but doesn't set the property to 'ALL'
        if (!m_activityList.isEmpty()) {
            m_activityList.clear();
            updateActivities(true);
        }
        return;
    }

    const QStringList newActivitiesList = prop.split(u',');

    if (newActivitiesList == m_activityList) {
        return; // expected change, it's ok.
    }

    setOnActivities(newActivitiesList);
#endif
}

void X11Window::checkActivities()
{
#if KWIN_BUILD_ACTIVITIES
    Xcb::StringProperty property = fetchActivities();
    readActivities(property);
#endif
}

void X11Window::setSessionActivityOverride(bool needed)
{
    sessionActivityOverride = needed;
    updateActivities(false);
}

Xcb::StringProperty X11Window::fetchPreferredColorScheme() const
{
    return Xcb::StringProperty(m_client, atoms->kde_color_sheme);
}

QString X11Window::readPreferredColorScheme(Xcb::StringProperty &property) const
{
    return rules()->checkDecoColor(QString::fromUtf8(property));
}

QString X11Window::preferredColorScheme() const
{
    Xcb::StringProperty property = fetchPreferredColorScheme();
    return readPreferredColorScheme(property);
}

bool X11Window::isClient() const
{
    return !m_unmanaged;
}

bool X11Window::isUnmanaged() const
{
    return m_unmanaged;
}

bool X11Window::isOutline() const
{
    return m_outline;
}

WindowType X11Window::windowType() const
{
    if (m_unmanaged) {
        return WindowType(info->windowType(SUPPORTED_UNMANAGED_WINDOW_TYPES_MASK));
    }

    WindowType wt = WindowType(info->windowType(SUPPORTED_MANAGED_WINDOW_TYPES_MASK));
    // hacks here
    if (wt == WindowType::Unknown) { // this is more or less suggested in NETWM spec
        wt = isTransient() ? WindowType::Dialog : WindowType::Normal;
    }
    return wt;
}

void X11Window::cancelFocusOutTimer()
{
    if (m_focusOutTimer) {
        m_focusOutTimer->stop();
    }
}

xcb_window_t X11Window::frameId() const
{
    return m_frame;
}

xcb_window_t X11Window::window() const
{
    return m_client;
}

xcb_window_t X11Window::wrapperId() const
{
    return m_wrapper;
}

QPointF X11Window::framePosToClientPos(const QPointF &point) const
{
    qreal x = point.x();
    qreal y = point.y();

    if (isDecorated()) {
        x += Xcb::nativeRound(borderLeft());
        y += Xcb::nativeRound(borderTop());
    } else {
        x -= m_clientFrameExtents.left();
        y -= m_clientFrameExtents.top();
    }

    return QPointF(x, y);
}

QPointF X11Window::clientPosToFramePos(const QPointF &point) const
{
    qreal x = point.x();
    qreal y = point.y();

    if (isDecorated()) {
        x -= Xcb::nativeRound(borderLeft());
        y -= Xcb::nativeRound(borderTop());
    } else {
        x += m_clientFrameExtents.left();
        y += m_clientFrameExtents.top();
    }

    return QPointF(x, y);
}

QSizeF X11Window::frameSizeToClientSize(const QSizeF &size) const
{
    qreal width = size.width();
    qreal height = size.height();

    if (isDecorated()) {
        // Both frameSize and clientSize are rounded to integral XNative units
        // So their difference must also be rounded to integral XNative units
        // Otherwise we get cycles of rounding that can cause growing window sizes
        width -= Xcb::nativeRound(borderLeft()) + Xcb::nativeRound(borderRight());
        height -= Xcb::nativeRound(borderTop()) + Xcb::nativeRound(borderBottom());
    } else {
        width += m_clientFrameExtents.left() + m_clientFrameExtents.right();
        height += m_clientFrameExtents.top() + m_clientFrameExtents.bottom();
    }

    return QSizeF(width, height);
}

QSizeF X11Window::clientSizeToFrameSize(const QSizeF &size) const
{
    qreal width = size.width();
    qreal height = size.height();

    if (isDecorated()) {
        // Both frameSize and clientSize are rounded to integral XNative units
        // So their difference must also be rounded to integral XNative units
        // Otherwise we get cycles of rounding that can cause growing window sizes
        width += Xcb::nativeRound(borderLeft()) + Xcb::nativeRound(borderRight());
        height += Xcb::nativeRound(borderTop()) + Xcb::nativeRound(borderBottom());
    } else {
        width -= m_clientFrameExtents.left() + m_clientFrameExtents.right();
        height -= m_clientFrameExtents.top() + m_clientFrameExtents.bottom();
    }

    return QSizeF(width, height);
}

QRectF X11Window::frameRectToBufferRect(const QRectF &rect) const
{
    if (!waylandServer() && isDecorated()) {
        return rect;
    }
    return frameRectToClientRect(rect);
}

/**
 * Returns the position of the wrapper window relative to the frame window. On X11, it
 * is the same as QPoint(borderLeft(), borderTop()). On Wayland, it's QPoint(0, 0).
 */
QPointF X11Window::wrapperPos() const
{
    return m_clientGeometry.topLeft() - m_bufferGeometry.topLeft();
}

/**
 * Returns the natural size of the window, if the window is not shaded it's the same
 * as size().
 */
QSizeF X11Window::implicitSize() const
{
    return clientSizeToFrameSize(m_client.geometry().size());
}

pid_t X11Window::pid() const
{
    return info->pid();
}

QString X11Window::windowRole() const
{
    return QString::fromLatin1(info->windowRole());
}

Xcb::Property X11Window::fetchShowOnScreenEdge() const
{
    return Xcb::Property(false, window(), atoms->kde_screen_edge_show, XCB_ATOM_CARDINAL, 0, 1);
}

void X11Window::readShowOnScreenEdge(Xcb::Property &property)
{
    const uint32_t value = property.value<uint32_t>(ElectricNone);
    ElectricBorder border = ElectricNone;
    switch (value & 0xFF) {
    case 0:
        border = ElectricTop;
        break;
    case 1:
        border = ElectricRight;
        break;
    case 2:
        border = ElectricBottom;
        break;
    case 3:
        border = ElectricLeft;
        break;
    }
    if (border != ElectricNone) {
        disconnect(m_edgeGeometryTrackingConnection);

        auto reserveScreenEdge = [this, border]() {
            if (workspace()->screenEdges()->reserve(this, border)) {
                setHidden(true);
            } else {
                setHidden(false);
            }
        };

        reserveScreenEdge();
        m_edgeGeometryTrackingConnection = connect(this, &X11Window::frameGeometryChanged, this, reserveScreenEdge);
    } else if (!property.isNull() && property->type != XCB_ATOM_NONE) {
        // property value is incorrect, delete the property
        // so that the client knows that it is not hidden
        xcb_delete_property(kwinApp()->x11Connection(), window(), atoms->kde_screen_edge_show);
    } else {
        // restore
        disconnect(m_edgeGeometryTrackingConnection);

        setHidden(false);

        workspace()->screenEdges()->reserve(this, ElectricNone);
    }
}

void X11Window::updateShowOnScreenEdge()
{
    Xcb::Property property = fetchShowOnScreenEdge();
    readShowOnScreenEdge(property);
}

void X11Window::showOnScreenEdge()
{
    setHidden(false);
    xcb_delete_property(kwinApp()->x11Connection(), window(), atoms->kde_screen_edge_show);
}

bool X11Window::belongsToSameApplication(const Window *other, SameApplicationChecks checks) const
{
    const X11Window *c2 = dynamic_cast<const X11Window *>(other);
    if (!c2) {
        return false;
    }
    return X11Window::belongToSameApplication(this, c2, checks);
}

QSizeF X11Window::resizeIncrements() const
{
    return m_geometryHints.resizeIncrements();
}

Xcb::StringProperty X11Window::fetchApplicationMenuServiceName() const
{
    return Xcb::StringProperty(m_client, atoms->kde_net_wm_appmenu_service_name);
}

void X11Window::readApplicationMenuServiceName(Xcb::StringProperty &property)
{
    updateApplicationMenuServiceName(QString::fromUtf8(property));
}

void X11Window::checkApplicationMenuServiceName()
{
    Xcb::StringProperty property = fetchApplicationMenuServiceName();
    readApplicationMenuServiceName(property);
}

Xcb::StringProperty X11Window::fetchApplicationMenuObjectPath() const
{
    return Xcb::StringProperty(m_client, atoms->kde_net_wm_appmenu_object_path);
}

void X11Window::readApplicationMenuObjectPath(Xcb::StringProperty &property)
{
    updateApplicationMenuObjectPath(QString::fromUtf8(property));
}

void X11Window::checkApplicationMenuObjectPath()
{
    Xcb::StringProperty property = fetchApplicationMenuObjectPath();
    readApplicationMenuObjectPath(property);
}

void X11Window::handleSync()
{
    setReadyForPainting();
    m_syncRequest.isPending = false;
    if (m_syncRequest.failsafeTimeout) {
        m_syncRequest.failsafeTimeout->stop();
    }

    // Sync request can be acknowledged shortly after finishing resize.
    if (m_syncRequest.interactiveResize) {
        m_syncRequest.interactiveResize = false;
        if (m_syncRequest.timeout) {
            m_syncRequest.timeout->stop();
        }
        performInteractiveResize();
        updateWindowPixmap();
    }
}

void X11Window::performInteractiveResize()
{
    resize(moveResizeGeometry().size());
}

bool X11Window::belongToSameApplication(const X11Window *c1, const X11Window *c2, SameApplicationChecks checks)
{
    bool same_app = false;

    // tests that definitely mean they belong together
    if (c1 == c2) {
        same_app = true;
    } else if (c1->isTransient() && c2->hasTransient(c1, true)) {
        same_app = true; // c1 has c2 as mainwindow
    } else if (c2->isTransient() && c1->hasTransient(c2, true)) {
        same_app = true; // c2 has c1 as mainwindow
    } else if (c1->group() == c2->group()) {
        same_app = true; // same group
    } else if (c1->wmClientLeader() == c2->wmClientLeader()
               && c1->wmClientLeader() != c1->window() // if WM_CLIENT_LEADER is not set, it returns window(),
               && c2->wmClientLeader() != c2->window()) { // don't use in this test then
        same_app = true; // same client leader

        // tests that mean they most probably don't belong together
    } else if ((c1->pid() != c2->pid() && !checks.testFlag(SameApplicationCheck::AllowCrossProcesses))
               || c1->wmClientMachine(false) != c2->wmClientMachine(false)) {
        ; // different processes
    } else if (c1->wmClientLeader() != c2->wmClientLeader()
               && c1->wmClientLeader() != c1->window() // if WM_CLIENT_LEADER is not set, it returns window(),
               && c2->wmClientLeader() != c2->window() // don't use in this test then
               && !checks.testFlag(SameApplicationCheck::AllowCrossProcesses)) {
        ; // different client leader
    } else if (c1->resourceClass() != c2->resourceClass()) {
        ; // different apps
    } else if (!sameAppWindowRoleMatch(c1, c2, checks.testFlag(SameApplicationCheck::RelaxedForActive))
               && !checks.testFlag(SameApplicationCheck::AllowCrossProcesses)) {
        ; // "different" apps
    } else if (c1->pid() == 0 || c2->pid() == 0) {
        ; // old apps that don't have _NET_WM_PID, consider them different
        // if they weren't found to match above
    } else {
        same_app = true; // looks like it's the same app
    }

    return same_app;
}

// Non-transient windows with window role containing '#' are always
// considered belonging to different applications (unless
// the window role is exactly the same). KMainWindow sets
// window role this way by default, and different KMainWindow
// usually "are" different application from user's point of view.
// This help with no-focus-stealing for e.g. konqy reusing.
// On the other hand, if one of the windows is active, they are
// considered belonging to the same application. This is for
// the cases when opening new mainwindow directly from the application,
// e.g. 'Open New Window' in konqy ( active_hack == true ).
bool X11Window::sameAppWindowRoleMatch(const X11Window *c1, const X11Window *c2, bool active_hack)
{
    if (c1->isTransient()) {
        while (const X11Window *t = dynamic_cast<const X11Window *>(c1->transientFor())) {
            c1 = t;
        }
        if (c1->groupTransient()) {
            return c1->group() == c2->group();
        }
#if 0
        // if a group transient is in its own group, it didn't possibly have a group,
        // and therefore should be considered belonging to the same app like
        // all other windows from the same app
        || c1->group()->leaderClient() == c1 || c2->group()->leaderClient() == c2;
#endif
    }
    if (c2->isTransient()) {
        while (const X11Window *t = dynamic_cast<const X11Window *>(c2->transientFor())) {
            c2 = t;
        }
        if (c2->groupTransient()) {
            return c1->group() == c2->group();
        }
#if 0
        || c1->group()->leaderClient() == c1 || c2->group()->leaderClient() == c2;
#endif
    }
    int pos1 = c1->windowRole().indexOf('#');
    int pos2 = c2->windowRole().indexOf('#');
    if ((pos1 >= 0 && pos2 >= 0)) {
        if (!active_hack) { // without the active hack for focus stealing prevention,
            return c1 == c2; // different mainwindows are always different apps
        }
        if (!c1->isActive() && !c2->isActive()) {
            return c1 == c2;
        } else {
            return true;
        }
    }
    return true;
}

/*

 Transiency stuff: ICCCM 4.1.2.6, NETWM 7.3

 WM_TRANSIENT_FOR is basically means "this is my mainwindow".
 For NET::Unknown windows, transient windows are considered to be NET::Dialog
 windows, for compatibility with non-NETWM clients. KWin may adjust the value
 of this property in some cases (window pointing to itself or creating a loop,
 keeping NET::Splash windows above other windows from the same app, etc.).

 X11Window::transient_for_id is the value of the WM_TRANSIENT_FOR property, after
 possibly being adjusted by KWin. X11Window::transient_for points to the Client
 this Client is transient for, or is NULL. If X11Window::transient_for_id is
 poiting to the root window, the window is considered to be transient
 for the whole window group, as suggested in NETWM 7.3.

 In the case of group transient window, X11Window::transient_for is NULL,
 and X11Window::groupTransient() returns true. Such window is treated as
 if it were transient for every window in its window group that has been
 mapped _before_ it (or, to be exact, was added to the same group before it).
 Otherwise two group transients can create loops, which can lead very very
 nasty things (bug #67914 and all its dupes).

 X11Window::original_transient_for_id is the value of the property, which
 may be different if X11Window::transient_for_id if e.g. forcing NET::Splash
 to be kept on top of its window group, or when the mainwindow is not mapped
 yet, in which case the window is temporarily made group transient,
 and when the mainwindow is mapped, transiency is re-evaluated.

 This can get a bit complicated with with e.g. two Konqueror windows created
 by the same process. They should ideally appear like two independent applications
 to the user. This should be accomplished by all windows in the same process
 having the same window group (needs to be changed in Qt at the moment), and
 using non-group transients poiting to their relevant mainwindow for toolwindows
 etc. KWin should handle both group and non-group transient dialogs well.

 In other words:
 - non-transient windows     : isTransient() == false
 - normal transients         : transientFor() != NULL
 - group transients          : groupTransient() == true

 - list of mainwindows       : mainClients()  (call once and loop over the result)
 - list of transients        : transients()
 - every window in the group : group()->members()
*/

Xcb::TransientFor X11Window::fetchTransient() const
{
    return Xcb::TransientFor(window());
}

void X11Window::readTransientProperty(Xcb::TransientFor &transientFor)
{
    xcb_window_t new_transient_for_id = XCB_WINDOW_NONE;
    if (transientFor.getTransientFor(&new_transient_for_id)) {
        m_originalTransientForId = new_transient_for_id;
        new_transient_for_id = verifyTransientFor(new_transient_for_id, true);
    } else {
        m_originalTransientForId = XCB_WINDOW_NONE;
        new_transient_for_id = verifyTransientFor(XCB_WINDOW_NONE, false);
    }
    setTransient(new_transient_for_id);
}

void X11Window::readTransient()
{
    if (isUnmanaged()) {
        return;
    }
    Xcb::TransientFor transientFor = fetchTransient();
    readTransientProperty(transientFor);
}

void X11Window::setTransient(xcb_window_t new_transient_for_id)
{
    if (new_transient_for_id != m_transientForId) {
        removeFromMainClients();
        X11Window *transient_for = nullptr;
        m_transientForId = new_transient_for_id;
        if (m_transientForId != XCB_WINDOW_NONE && !groupTransient()) {
            transient_for = workspace()->findClient(Predicate::WindowMatch, m_transientForId);
            Q_ASSERT(transient_for != nullptr); // verifyTransient() had to check this
            transient_for->addTransient(this);
        } // checkGroup() will check 'check_active_modal'
        setTransientFor(transient_for);
        checkGroup(nullptr, true); // force, because transiency has changed
        updateLayer();
        workspace()->resetUpdateToolWindowsTimer();
        Q_EMIT transientChanged();
    }
}

void X11Window::removeFromMainClients()
{
    if (transientFor()) {
        transientFor()->removeTransient(this);
    }
    if (groupTransient()) {
        for (auto it = group()->members().constBegin(); it != group()->members().constEnd(); ++it) {
            (*it)->removeTransient(this);
        }
    }
}

// *sigh* this transiency handling is madness :(
// This one is called when destroying/releasing a window.
// It makes sure this client is removed from all grouping
// related lists.
void X11Window::cleanGrouping()
{
    // We want to break parent-child relationships, but preserve stacking
    // order constraints at the same time for window closing animations.

    if (transientFor()) {
        transientFor()->removeTransientFromList(this);
        setTransientFor(nullptr);
    }

    if (groupTransient()) {
        const auto members = group()->members();
        for (Window *member : members) {
            member->removeTransientFromList(this);
        }
    }

    const auto children = transients();
    for (Window *transient : children) {
        removeTransientFromList(transient);
        transient->setTransientFor(nullptr);
    }

    group()->removeMember(this);
    in_group = nullptr;
    m_transientForId = XCB_WINDOW_NONE;
}

// Make sure that no group transient is considered transient
// for a window that is (directly or indirectly) transient for it
// (including another group transients).
// Non-group transients not causing loops are checked in verifyTransientFor().
void X11Window::checkGroupTransients()
{
    for (auto it1 = group()->members().constBegin(); it1 != group()->members().constEnd(); ++it1) {
        if (!(*it1)->groupTransient()) { // check all group transients in the group
            continue; // TODO optimize to check only the changed ones?
        }
        for (auto it2 = group()->members().constBegin(); it2 != group()->members().constEnd(); ++it2) { // group transients can be transient only for others in the group,
            // so don't make them transient for the ones that are transient for it
            if (*it1 == *it2) {
                continue;
            }
            for (Window *cl = (*it2)->transientFor(); cl != nullptr; cl = cl->transientFor()) {
                if (cl == *it1) {
                    // don't use removeTransient(), that would modify *it2 too
                    (*it2)->removeTransientFromList(*it1);
                    continue;
                }
            }
            // if *it1 and *it2 are both group transients, and are transient for each other,
            // make only *it2 transient for *it1 (i.e. subwindow), as *it2 came later,
            // and should be therefore on top of *it1
            // TODO This could possibly be optimized, it also requires hasTransient() to check for loops.
            if ((*it2)->groupTransient() && (*it1)->hasTransient(*it2, true) && (*it2)->hasTransient(*it1, true)) {
                (*it2)->removeTransientFromList(*it1);
            }
            // if there are already windows W1 and W2, W2 being transient for W1, and group transient W3
            // is added, make it transient only for W2, not for W1, because it's already indirectly
            // transient for it - the indirect transiency actually shouldn't break anything,
            // but it can lead to exponentially expensive operations (#95231)
            // TODO this is pretty slow as well
            for (auto it3 = group()->members().constBegin(); it3 != group()->members().constEnd(); ++it3) {
                if (*it1 == *it2 || *it2 == *it3 || *it1 == *it3) {
                    continue;
                }
                if ((*it2)->hasTransient(*it1, false) && (*it3)->hasTransient(*it1, false)) {
                    if ((*it2)->hasTransient(*it3, true)) {
                        (*it2)->removeTransientFromList(*it1);
                    }
                    if ((*it3)->hasTransient(*it2, true)) {
                        (*it3)->removeTransientFromList(*it1);
                    }
                }
            }
        }
    }
}

/**
 * Check that the window is not transient for itself, and similar nonsense.
 */
xcb_window_t X11Window::verifyTransientFor(xcb_window_t new_transient_for, bool set)
{
    xcb_window_t new_property_value = new_transient_for;
    // make sure splashscreens are shown above all their app's windows, even though
    // they're in Normal layer
    if (isSplash() && new_transient_for == XCB_WINDOW_NONE) {
        new_transient_for = kwinApp()->x11RootWindow();
    }
    if (new_transient_for == XCB_WINDOW_NONE) {
        if (set) { // sometimes WM_TRANSIENT_FOR is set to None, instead of root window
            new_property_value = new_transient_for = kwinApp()->x11RootWindow();
        } else {
            return XCB_WINDOW_NONE;
        }
    }
    if (new_transient_for == window()) { // pointing to self
        // also fix the property itself
        qCWarning(KWIN_CORE) << "Client " << this << " has WM_TRANSIENT_FOR poiting to itself.";
        new_property_value = new_transient_for = kwinApp()->x11RootWindow();
    }
    //  The transient_for window may be embedded in another application,
    //  so kwin cannot see it. Try to find the managed client for the
    //  window and fix the transient_for property if possible.
    xcb_window_t before_search = new_transient_for;
    while (new_transient_for != XCB_WINDOW_NONE
           && new_transient_for != kwinApp()->x11RootWindow()
           && !workspace()->findClient(Predicate::WindowMatch, new_transient_for)) {
        Xcb::Tree tree(new_transient_for);
        if (tree.isNull()) {
            break;
        }
        new_transient_for = tree->parent;
    }
    if (X11Window *new_transient_for_client = workspace()->findClient(Predicate::WindowMatch, new_transient_for)) {
        if (new_transient_for != before_search) {
            qCDebug(KWIN_CORE) << "Client " << this << " has WM_TRANSIENT_FOR poiting to non-toplevel window "
                               << before_search << ", child of " << new_transient_for_client << ", adjusting.";
            new_property_value = new_transient_for; // also fix the property
        }
    } else {
        new_transient_for = before_search; // nice try
    }
    // loop detection
    // group transients cannot cause loops, because they're considered transient only for non-transient
    // windows in the group
    int count = 20;
    xcb_window_t loop_pos = new_transient_for;
    while (loop_pos != XCB_WINDOW_NONE && loop_pos != kwinApp()->x11RootWindow()) {
        X11Window *pos = workspace()->findClient(Predicate::WindowMatch, loop_pos);
        if (pos == nullptr) {
            break;
        }
        loop_pos = pos->m_transientForId;
        if (--count == 0 || pos == this) {
            qCWarning(KWIN_CORE) << "Client " << this << " caused WM_TRANSIENT_FOR loop.";
            new_transient_for = kwinApp()->x11RootWindow();
        }
    }
    if (new_transient_for != kwinApp()->x11RootWindow()
        && workspace()->findClient(Predicate::WindowMatch, new_transient_for) == nullptr) {
        // it's transient for a specific window, but that window is not mapped
        new_transient_for = kwinApp()->x11RootWindow();
    }
    if (new_property_value != m_originalTransientForId) {
        Xcb::setTransientFor(window(), new_property_value);
    }
    return new_transient_for;
}

void X11Window::addTransient(Window *cl)
{
    Window::addTransient(cl);
    if (workspace()->mostRecentlyActivatedWindow() == this && cl->isModal()) {
        check_active_modal = true;
    }
}

// A new window has been mapped. Check if it's not a mainwindow for this already existing window.
void X11Window::checkTransient(xcb_window_t w)
{
    if (m_originalTransientForId != w) {
        return;
    }
    w = verifyTransientFor(w, true);
    setTransient(w);
}

// returns true if cl is the transient_for window for this client,
// or recursively the transient_for window
bool X11Window::hasTransient(const Window *cl, bool indirect) const
{
    if (const X11Window *c = dynamic_cast<const X11Window *>(cl)) {
        // checkGroupTransients() uses this to break loops, so hasTransient() must detect them
        QList<const X11Window *> set;
        return hasTransientInternal(c, indirect, set);
    }
    return false;
}

bool X11Window::hasTransientInternal(const X11Window *cl, bool indirect, QList<const X11Window *> &set) const
{
    if (const X11Window *t = dynamic_cast<const X11Window *>(cl->transientFor())) {
        if (t == this) {
            return true;
        }
        if (!indirect) {
            return false;
        }
        if (set.contains(cl)) {
            return false;
        }
        set.append(cl);
        return hasTransientInternal(t, indirect, set);
    }
    if (!cl->isTransient()) {
        return false;
    }
    if (group() != cl->group()) {
        return false;
    }
    // cl is group transient, search from top
    if (transients().contains(cl)) {
        return true;
    }
    if (!indirect) {
        return false;
    }
    if (set.contains(this)) {
        return false;
    }
    set.append(this);
    for (auto it = transients().constBegin(); it != transients().constEnd(); ++it) {
        const X11Window *c = qobject_cast<const X11Window *>(*it);
        if (!c) {
            continue;
        }
        if (c->hasTransientInternal(cl, indirect, set)) {
            return true;
        }
    }
    return false;
}

QList<Window *> X11Window::mainWindows() const
{
    if (!isTransient()) {
        return QList<Window *>();
    }
    if (const Window *t = transientFor()) {
        return QList<Window *>{const_cast<Window *>(t)};
    }
    QList<Window *> result;
    Q_ASSERT(group());
    for (auto it = group()->members().constBegin(); it != group()->members().constEnd(); ++it) {
        if ((*it)->hasTransient(this, false)) {
            result.append(*it);
        }
    }
    return result;
}

// X11Window::window_group only holds the contents of the hint,
// but it should be used only to find the group, not for anything else
// Argument is only when some specific group needs to be set.
void X11Window::checkGroup(Group *set_group, bool force)
{
    Group *old_group = in_group;
    if (old_group != nullptr) {
        old_group->ref(); // turn off automatic deleting
    }
    if (set_group != nullptr) {
        if (set_group != in_group) {
            if (in_group != nullptr) {
                in_group->removeMember(this);
            }
            in_group = set_group;
            in_group->addMember(this);
        }
    } else if (info->groupLeader() != XCB_WINDOW_NONE) {
        Group *new_group = workspace()->findGroup(info->groupLeader());
        X11Window *t = qobject_cast<X11Window *>(transientFor());
        if (t != nullptr && t->group() != new_group) {
            // move the window to the right group (e.g. a dialog provided
            // by different app, but transient for this one, so make it part of that group)
            new_group = t->group();
        }
        if (new_group == nullptr) { // doesn't exist yet
            new_group = new Group(info->groupLeader());
        }
        if (new_group != in_group) {
            if (in_group != nullptr) {
                in_group->removeMember(this);
            }
            in_group = new_group;
            in_group->addMember(this);
        }
    } else {
        if (X11Window *t = qobject_cast<X11Window *>(transientFor())) {
            // doesn't have window group set, but is transient for something
            // so make it part of that group
            Group *new_group = t->group();
            if (new_group != in_group) {
                if (in_group != nullptr) {
                    in_group->removeMember(this);
                }
                in_group = t->group();
                in_group->addMember(this);
            }
        } else if (groupTransient()) {
            // group transient which actually doesn't have a group :(
            // try creating group with other windows with the same client leader
            Group *new_group = workspace()->findClientLeaderGroup(this);
            if (new_group == nullptr) {
                new_group = new Group(XCB_WINDOW_NONE);
            }
            if (new_group != in_group) {
                if (in_group != nullptr) {
                    in_group->removeMember(this);
                }
                in_group = new_group;
                in_group->addMember(this);
            }
        } else { // Not transient without a group, put it in its client leader group.
            // This might be stupid if grouping was used for e.g. taskbar grouping
            // or minimizing together the whole group, but as long as it is used
            // only for dialogs it's better to keep windows from one app in one group.
            Group *new_group = workspace()->findClientLeaderGroup(this);
            if (in_group != nullptr && in_group != new_group) {
                in_group->removeMember(this);
                in_group = nullptr;
            }
            if (new_group == nullptr) {
                new_group = new Group(XCB_WINDOW_NONE);
            }
            if (in_group != new_group) {
                in_group = new_group;
                in_group->addMember(this);
            }
        }
    }
    if (in_group != old_group || force) {
        for (auto it = transients().constBegin(); it != transients().constEnd();) {
            auto *c = *it;
            // group transients in the old group are no longer transient for it
            if (c->groupTransient() && c->group() != group()) {
                removeTransientFromList(c);
                it = transients().constBegin(); // restart, just in case something more has changed with the list
            } else {
                ++it;
            }
        }
        if (groupTransient()) {
            // no longer transient for ones in the old group
            if (old_group != nullptr) {
                for (auto it = old_group->members().constBegin(); it != old_group->members().constEnd(); ++it) {
                    (*it)->removeTransient(this);
                }
            }
            // and make transient for all in the new group
            for (auto it = group()->members().constBegin(); it != group()->members().constEnd(); ++it) {
                if (*it == this) {
                    break; // this means the window is only transient for windows mapped before it
                }
                (*it)->addTransient(this);
            }
        }
        // group transient splashscreens should be transient even for windows
        // in group mapped later
        for (auto it = group()->members().constBegin(); it != group()->members().constEnd(); ++it) {
            if (!(*it)->isSplash()) {
                continue;
            }
            if (!(*it)->groupTransient()) {
                continue;
            }
            if (*it == this || hasTransient(*it, true)) { // TODO indirect?
                continue;
            }
            addTransient(*it);
        }
    }
    if (old_group != nullptr) {
        old_group->deref(); // can be now deleted if empty
    }
    checkGroupTransients();
    checkActiveModal();
    updateLayer();
}

// used by Workspace::findClientLeaderGroup()
void X11Window::changeClientLeaderGroup(Group *gr)
{
    // transientFor() != NULL are in the group of their mainwindow, so keep them there
    if (transientFor() != nullptr) {
        return;
    }
    // also don't change the group for window which have group set
    if (info->groupLeader()) {
        return;
    }
    checkGroup(gr); // change group
}

bool X11Window::check_active_modal = false;

void X11Window::checkActiveModal()
{
    // if the active window got new modal transient, activate it.
    // cannot be done in AddTransient(), because there may temporarily
    // exist loops, breaking findModal
    X11Window *check_modal = dynamic_cast<X11Window *>(workspace()->mostRecentlyActivatedWindow());
    if (check_modal != nullptr && check_modal->check_active_modal) {
        X11Window *new_modal = dynamic_cast<X11Window *>(check_modal->findModal());
        if (new_modal != nullptr && new_modal != check_modal) {
            if (!new_modal->isManaged()) {
                return; // postpone check until end of manage()
            }
            workspace()->activateWindow(new_modal);
        }
        check_modal->check_active_modal = false;
    }
}

QSizeF X11Window::constrainClientSize(const QSizeF &size, SizeMode mode) const
{
    qreal w = size.width();
    qreal h = size.height();

    if (w < 1) {
        w = 1;
    }
    if (h < 1) {
        h = 1;
    }

    // basesize, minsize, maxsize, paspect and resizeinc have all values defined,
    // even if they're not set in flags - see getWmNormalHints()
    QSizeF min_size = minSize();
    QSizeF max_size = maxSize();
    if (isDecorated()) {
        QSizeF decominsize(0, 0);
        QSizeF border_size(borderLeft() + borderRight(), borderTop() + borderBottom());
        if (border_size.width() > decominsize.width()) { // just in case
            decominsize.setWidth(border_size.width());
        }
        if (border_size.height() > decominsize.height()) {
            decominsize.setHeight(border_size.height());
        }
        if (decominsize.width() > min_size.width()) {
            min_size.setWidth(decominsize.width());
        }
        if (decominsize.height() > min_size.height()) {
            min_size.setHeight(decominsize.height());
        }
    }
    w = std::min(max_size.width(), w);
    h = std::min(max_size.height(), h);
    w = std::max(min_size.width(), w);
    h = std::max(min_size.height(), h);

    const bool isX11Mode = kwinApp()->operationMode() == Application::OperationModeX11;
    if (!rules()->checkStrictGeometry(!isFullScreen() && isX11Mode)) {
        // Disobey increments and aspect by explicit rule.
        return QSizeF(w, h);
    }

    qreal width_inc = m_geometryHints.resizeIncrements().width();
    qreal height_inc = m_geometryHints.resizeIncrements().height();
    qreal basew_inc = m_geometryHints.baseSize().width();
    qreal baseh_inc = m_geometryHints.baseSize().height();
    if (!m_geometryHints.hasBaseSize()) {
        basew_inc = m_geometryHints.minSize().width();
        baseh_inc = m_geometryHints.minSize().height();
    }

    w = std::floor((w - basew_inc) / width_inc) * width_inc + basew_inc;
    h = std::floor((h - baseh_inc) / height_inc) * height_inc + baseh_inc;

    // code for aspect ratios based on code from FVWM
    /*
     * The math looks like this:
     *
     * minAspectX    dwidth     maxAspectX
     * ---------- <= ------- <= ----------
     * minAspectY    dheight    maxAspectY
     *
     * If that is multiplied out, then the width and height are
     * invalid in the following situations:
     *
     * minAspectX * dheight > minAspectY * dwidth
     * maxAspectX * dheight < maxAspectY * dwidth
     *
     */
    if (m_geometryHints.hasAspect()) {
        double min_aspect_w = m_geometryHints.minAspect().width(); // use doubles, because the values can be MAX_INT
        double min_aspect_h = m_geometryHints.minAspect().height(); // and multiplying would go wrong otherwise
        double max_aspect_w = m_geometryHints.maxAspect().width();
        double max_aspect_h = m_geometryHints.maxAspect().height();
        // According to ICCCM 4.1.2.3 PMinSize should be a fallback for PBaseSize for size increments,
        // but not for aspect ratio. Since this code comes from FVWM, handles both at the same time,
        // and I have no idea how it works, let's hope nobody relies on that.
        const QSizeF baseSize = m_geometryHints.baseSize();
        w -= baseSize.width();
        h -= baseSize.height();
        qreal max_width = max_size.width() - baseSize.width();
        qreal min_width = min_size.width() - baseSize.width();
        qreal max_height = max_size.height() - baseSize.height();
        qreal min_height = min_size.height() - baseSize.height();
#define ASPECT_CHECK_GROW_W                                                           \
    if (min_aspect_w * h > min_aspect_h * w) {                                        \
        int delta = int(min_aspect_w * h / min_aspect_h - w) / width_inc * width_inc; \
        if (w + delta <= max_width)                                                   \
            w += delta;                                                               \
    }
#define ASPECT_CHECK_SHRINK_H_GROW_W                                                      \
    if (min_aspect_w * h > min_aspect_h * w) {                                            \
        int delta = int(h - w * min_aspect_h / min_aspect_w) / height_inc * height_inc;   \
        if (h - delta >= min_height)                                                      \
            h -= delta;                                                                   \
        else {                                                                            \
            int delta = int(min_aspect_w * h / min_aspect_h - w) / width_inc * width_inc; \
            if (w + delta <= max_width)                                                   \
                w += delta;                                                               \
        }                                                                                 \
    }
#define ASPECT_CHECK_GROW_H                                                             \
    if (max_aspect_w * h < max_aspect_h * w) {                                          \
        int delta = int(w * max_aspect_h / max_aspect_w - h) / height_inc * height_inc; \
        if (h + delta <= max_height)                                                    \
            h += delta;                                                                 \
    }
#define ASPECT_CHECK_SHRINK_W_GROW_H                                                        \
    if (max_aspect_w * h < max_aspect_h * w) {                                              \
        int delta = int(w - max_aspect_w * h / max_aspect_h) / width_inc * width_inc;       \
        if (w - delta >= min_width)                                                         \
            w -= delta;                                                                     \
        else {                                                                              \
            int delta = int(w * max_aspect_h / max_aspect_w - h) / height_inc * height_inc; \
            if (h + delta <= max_height)                                                    \
                h += delta;                                                                 \
        }                                                                                   \
    }
        switch (mode) {
        case SizeModeAny:
#if 0 // make SizeModeAny equal to SizeModeFixedW - prefer keeping fixed width,
      // so that changing aspect ratio to a different value and back keeps the same size (#87298)
            {
                ASPECT_CHECK_SHRINK_H_GROW_W
                ASPECT_CHECK_SHRINK_W_GROW_H
                ASPECT_CHECK_GROW_H
                ASPECT_CHECK_GROW_W
                break;
            }
#endif
        case SizeModeFixedW: {
            // the checks are order so that attempts to modify height are first
            ASPECT_CHECK_GROW_H
            ASPECT_CHECK_SHRINK_H_GROW_W
            ASPECT_CHECK_SHRINK_W_GROW_H
            ASPECT_CHECK_GROW_W
            break;
        }
        case SizeModeFixedH: {
            ASPECT_CHECK_GROW_W
            ASPECT_CHECK_SHRINK_W_GROW_H
            ASPECT_CHECK_SHRINK_H_GROW_W
            ASPECT_CHECK_GROW_H
            break;
        }
        case SizeModeMax: {
            // first checks that try to shrink
            ASPECT_CHECK_SHRINK_H_GROW_W
            ASPECT_CHECK_SHRINK_W_GROW_H
            ASPECT_CHECK_GROW_W
            ASPECT_CHECK_GROW_H
            break;
        }
        }
#undef ASPECT_CHECK_SHRINK_H_GROW_W
#undef ASPECT_CHECK_SHRINK_W_GROW_H
#undef ASPECT_CHECK_GROW_W
#undef ASPECT_CHECK_GROW_H
        w += baseSize.width();
        h += baseSize.height();
    }

    return QSizeF(w, h);
}

void X11Window::getResourceClass()
{
    setResourceClass(QString::fromLatin1(info->windowClassName()), QString::fromLatin1(info->windowClassClass()));
}

/**
 * Gets the client's normal WM hints and reconfigures itself respectively.
 */
void X11Window::getWmNormalHints()
{
    if (isUnmanaged()) {
        return;
    }
    const bool hadFixedAspect = m_geometryHints.hasAspect();
    // roundtrip to X server
    m_geometryHints.fetch();
    m_geometryHints.read();

    if (!hadFixedAspect && m_geometryHints.hasAspect()) {
        // align to eventual new constraints
        maximize(max_mode);
    }
    if (isManaged()) {
        // update to match restrictions
        QSizeF new_size = clientSizeToFrameSize(constrainClientSize(clientSize()));
        if (new_size != size() && !isFullScreen()) {
            QRectF origClientGeometry = m_clientGeometry;
            moveResize(resizeWithChecks(moveResizeGeometry(), new_size));
            if ((!isSpecialWindow() || isToolbar()) && !isFullScreen()) {
                // try to keep the window in its xinerama screen if possible,
                // if that fails at least keep it visible somewhere
                QRectF area = workspace()->clientArea(MovementArea, this, moveResizeOutput());
                if (area.contains(origClientGeometry)) {
                    moveResize(keepInArea(moveResizeGeometry(), area));
                }
                area = workspace()->clientArea(WorkArea, this, moveResizeOutput());
                if (area.contains(origClientGeometry)) {
                    moveResize(keepInArea(moveResizeGeometry(), area));
                }
            }
        }
    }
    updateAllowedActions(); // affects isResizeable()
}

QSizeF X11Window::minSize() const
{
    return rules()->checkMinSize(m_geometryHints.minSize());
}

QSizeF X11Window::maxSize() const
{
    return rules()->checkMaxSize(m_geometryHints.maxSize());
}

QSizeF X11Window::basicUnit() const
{
    const bool isX11Mode = kwinApp()->operationMode() == Application::OperationModeX11;
    if (!isX11Mode) {
        return QSize(1, 1);
    }
    return m_geometryHints.resizeIncrements();
}

/**
 * Auxiliary function to inform the client about the current window
 * configuration.
 */
void X11Window::sendSyntheticConfigureNotify()
{
    // Every X11 event is 32 bytes (see man xcb_send_event), so XCB will copy
    // 32 unconditionally. Use a union to ensure we don't disclose stack memory.
    union {
        xcb_configure_notify_event_t event;
        char buffer[32];
    } u;
    static_assert(sizeof(u.event) < 32, "wouldn't need the union otherwise");
    memset(&u, 0, sizeof(u));
    xcb_configure_notify_event_t &c = u.event;
    u.event.response_type = XCB_CONFIGURE_NOTIFY;
    u.event.event = window();
    u.event.window = window();
    u.event.x = Xcb::toXNative(m_clientGeometry.x());
    u.event.y = Xcb::toXNative(m_clientGeometry.y());
    u.event.width = Xcb::toXNative(m_clientGeometry.width());
    u.event.height = Xcb::toXNative(m_clientGeometry.height());
    u.event.border_width = 0;
    u.event.above_sibling = XCB_WINDOW_NONE;
    u.event.override_redirect = 0;
    xcb_send_event(kwinApp()->x11Connection(), true, c.event, XCB_EVENT_MASK_STRUCTURE_NOTIFY, reinterpret_cast<const char *>(&u));
    xcb_flush(kwinApp()->x11Connection());
}

void X11Window::handleXwaylandScaleChanged()
{
    // while KWin implicitly considers the window already resized when the scale changes,
    // this is needed to make Xwayland actually resize it as well
    resize(moveResizeGeometry().size());
}

QPointF X11Window::gravityAdjustment(xcb_gravity_t gravity) const
{
    qreal dx = 0;
    qreal dy = 0;

    // dx, dy specify how the client window moves to make space for the frame.
    // In general we have to compute the reference point and from that figure
    // out how much we need to shift the client, however given that we ignore
    // the border width attribute and the extents of the server-side decoration
    // are known in advance, we can simplify the math quite a bit and express
    // the required window gravity adjustment in terms of border sizes.
    switch (gravity) {
    case XCB_GRAVITY_NORTH_WEST: // move down right
    default:
        dx = borderLeft();
        dy = borderTop();
        break;
    case XCB_GRAVITY_NORTH: // move right
        dx = 0;
        dy = borderTop();
        break;
    case XCB_GRAVITY_NORTH_EAST: // move down left
        dx = -borderRight();
        dy = borderTop();
        break;
    case XCB_GRAVITY_WEST: // move right
        dx = borderLeft();
        dy = 0;
        break;
    case XCB_GRAVITY_CENTER:
        dx = (borderLeft() - borderRight()) / 2;
        dy = (borderTop() - borderBottom()) / 2;
        break;
    case XCB_GRAVITY_STATIC: // don't move
        dx = 0;
        dy = 0;
        break;
    case XCB_GRAVITY_EAST: // move left
        dx = -borderRight();
        dy = 0;
        break;
    case XCB_GRAVITY_SOUTH_WEST: // move up right
        dx = borderLeft();
        dy = -borderBottom();
        break;
    case XCB_GRAVITY_SOUTH: // move up
        dx = 0;
        dy = -borderBottom();
        break;
    case XCB_GRAVITY_SOUTH_EAST: // move up left
        dx = -borderRight();
        dy = -borderBottom();
        break;
    }

    return QPoint(dx, dy);
}

const QPointF X11Window::calculateGravitation(bool invert) const
{
    const QPointF adjustment = gravityAdjustment(m_geometryHints.windowGravity());

    // translate from client movement to frame movement
    const qreal dx = adjustment.x() - borderLeft();
    const qreal dy = adjustment.y() - borderTop();

    if (!invert) {
        return QPointF(x() + dx, y() + dy);
    } else {
        return QPointF(x() - dx, y() - dy);
    }
}

// co-ordinate are in kwin logical
void X11Window::configureRequest(int value_mask, qreal rx, qreal ry, qreal rw, qreal rh, int gravity, bool from_tool)
{
    const int configurePositionMask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y;
    const int configureSizeMask = XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT;
    const int configureGeometryMask = configurePositionMask | configureSizeMask;

    // "maximized" is a user setting -> we do not allow the client to resize itself
    // away from this & against the users explicit wish
    qCDebug(KWIN_CORE) << this << bool(value_mask & configureGeometryMask) << bool(maximizeMode() & MaximizeVertical) << bool(maximizeMode() & MaximizeHorizontal);

    // we want to (partially) ignore the request when the window is somehow maximized or quicktiled
    bool ignore = !app_noborder && (quickTileMode() != QuickTileMode(QuickTileFlag::None) || maximizeMode() != MaximizeRestore);
    // however, the user shall be able to force obedience despite and also disobedience in general
    ignore = rules()->checkIgnoreGeometry(ignore);
    if (!ignore) { // either we're not max'd / q'tiled or the user allowed the client to break that - so break it.
        updateQuickTileMode(QuickTileFlag::None);
        max_mode = MaximizeRestore;
        Q_EMIT quickTileModeChanged();
    } else if (!app_noborder && quickTileMode() == QuickTileMode(QuickTileFlag::None) && (maximizeMode() == MaximizeVertical || maximizeMode() == MaximizeHorizontal)) {
        // ignoring can be, because either we do, or the user does explicitly not want it.
        // for partially maximized windows we want to allow configures in the other dimension.
        // so we've to ask the user again - to know whether we just ignored for the partial maximization.
        // the problem here is, that the user can explicitly permit configure requests - even for maximized windows!
        // we cannot distinguish that from passing "false" for partially maximized windows.
        ignore = rules()->checkIgnoreGeometry(false);
        if (!ignore) { // the user is not interested, so we fix up dimensions
            if (maximizeMode() == MaximizeVertical) {
                value_mask &= ~(XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_HEIGHT);
            }
            if (maximizeMode() == MaximizeHorizontal) {
                value_mask &= ~(XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_WIDTH);
            }
            if (!(value_mask & configureGeometryMask)) {
                ignore = true; // the modification turned the request void
            }
        }
    }

    if (ignore) {
        qCDebug(KWIN_CORE) << "DENIED";
        return; // nothing to (left) to do for use - bugs #158974, #252314, #321491
    }

    qCDebug(KWIN_CORE) << "PERMITTED" << this << bool(value_mask & configureGeometryMask);

    if (gravity == 0) { // default (nonsense) value for the argument
        gravity = m_geometryHints.windowGravity();
    }
    if (value_mask & configurePositionMask) {
        QPointF new_pos = framePosToClientPos(pos());
        new_pos -= gravityAdjustment(xcb_gravity_t(gravity));
        if (value_mask & XCB_CONFIG_WINDOW_X) {
            new_pos.setX(rx);
        }
        if (value_mask & XCB_CONFIG_WINDOW_Y) {
            new_pos.setY(ry);
        }
        new_pos += gravityAdjustment(xcb_gravity_t(gravity));
        new_pos = clientPosToFramePos(new_pos);

        qreal nw = clientSize().width();
        qreal nh = clientSize().height();
        if (value_mask & XCB_CONFIG_WINDOW_WIDTH) {
            nw = rw;
        }
        if (value_mask & XCB_CONFIG_WINDOW_HEIGHT) {
            nh = rh;
        }
        const QSizeF requestedClientSize = constrainClientSize(QSizeF(nw, nh));
        QSizeF requestedFrameSize = clientSizeToFrameSize(requestedClientSize);
        requestedFrameSize = rules()->checkSize(requestedFrameSize);
        new_pos = rules()->checkPosition(new_pos);

        Output *newOutput = workspace()->outputAt(QRectF(new_pos, requestedFrameSize).center());
        if (newOutput != rules()->checkOutput(newOutput)) {
            return; // not allowed by rule
        }

        QRectF geometry = QRectF(new_pos, requestedFrameSize);
        const QRectF area = workspace()->clientArea(WorkArea, this, geometry.center());
        if (!from_tool && (!isSpecialWindow() || isToolbar()) && !isFullScreen() && area.contains(clientGeometry())) {
            geometry = keepInArea(geometry, area);
        }

        moveResize(geometry);

        // this is part of the kicker-xinerama-hack... it should be
        // safe to remove when kicker gets proper ExtendedStrut support;
        // see Workspace::rearrange() and X11Window::adjustedClientArea()
        if (hasStrut()) {
            workspace()->rearrange();
        }
    }

    if (value_mask & configureSizeMask && !(value_mask & configurePositionMask)) { // pure resize
        qreal nw = clientSize().width();
        qreal nh = clientSize().height();
        if (value_mask & XCB_CONFIG_WINDOW_WIDTH) {
            nw = rw;
        }
        if (value_mask & XCB_CONFIG_WINDOW_HEIGHT) {
            nh = rh;
        }

        const QSizeF requestedClientSize = constrainClientSize(QSizeF(nw, nh));
        const QSizeF requestedFrameSize = rules()->checkSize(clientSizeToFrameSize(requestedClientSize));

        if (requestedFrameSize != size()) { // don't restore if some app sets its own size again
            QRectF geometry = resizeWithChecks(moveResizeGeometry(), requestedFrameSize, xcb_gravity_t(gravity));
            if (!from_tool && (!isSpecialWindow() || isToolbar()) && !isFullScreen()) {
                // try to keep the window in its xinerama screen if possible,
                // if that fails at least keep it visible somewhere
                QRectF area = workspace()->clientArea(MovementArea, this, geometry.center());
                if (area.contains(clientGeometry())) {
                    geometry = keepInArea(geometry, area);
                }
                area = workspace()->clientArea(WorkArea, this, geometry.center());
                if (area.contains(clientGeometry())) {
                    geometry = keepInArea(geometry, area);
                }
            }
            moveResize(geometry);
        }
    }
    // No need to send synthetic configure notify event here, either it's sent together
    // with geometry change, or there's no need to send it.
    // Handling of the real ConfigureRequest event forces sending it, as there it's necessary.
}

QRectF X11Window::resizeWithChecks(const QRectF &geometry, qreal w, qreal h, xcb_gravity_t gravity)
{
    Q_ASSERT(!shade_geometry_change);
    if (isShade()) {
        if (h == borderTop() + borderBottom()) {
            qCWarning(KWIN_CORE) << "Shaded geometry passed for size:";
        }
    }
    qreal newx = geometry.x();
    qreal newy = geometry.y();
    QRectF area = workspace()->clientArea(WorkArea, this, geometry.center());
    // don't allow growing larger than workarea
    if (w > area.width()) {
        w = area.width();
    }
    if (h > area.height()) {
        h = area.height();
    }
    QSizeF tmp = constrainFrameSize(QSizeF(w, h)); // checks size constraints, including min/max size
    w = tmp.width();
    h = tmp.height();
    if (gravity == 0) {
        gravity = m_geometryHints.windowGravity();
    }
    switch (gravity) {
    case XCB_GRAVITY_NORTH_WEST: // top left corner doesn't move
    default:
        break;
    case XCB_GRAVITY_NORTH: // middle of top border doesn't move
        newx = (newx + geometry.width() / 2) - (w / 2);
        break;
    case XCB_GRAVITY_NORTH_EAST: // top right corner doesn't move
        newx = newx + geometry.width() - w;
        break;
    case XCB_GRAVITY_WEST: // middle of left border doesn't move
        newy = (newy + geometry.height() / 2) - (h / 2);
        break;
    case XCB_GRAVITY_CENTER: // middle point doesn't move
        newx = (newx + geometry.width() / 2) - (w / 2);
        newy = (newy + geometry.height() / 2) - (h / 2);
        break;
    case XCB_GRAVITY_STATIC: // top left corner of _client_ window doesn't move
        // since decoration doesn't change, equal to NorthWestGravity
        break;
    case XCB_GRAVITY_EAST: // // middle of right border doesn't move
        newx = newx + geometry.width() - w;
        newy = (newy + geometry.height() / 2) - (h / 2);
        break;
    case XCB_GRAVITY_SOUTH_WEST: // bottom left corner doesn't move
        newy = newy + geometry.height() - h;
        break;
    case XCB_GRAVITY_SOUTH: // middle of bottom border doesn't move
        newx = (newx + geometry.width() / 2) - (w / 2);
        newy = newy + geometry.height() - h;
        break;
    case XCB_GRAVITY_SOUTH_EAST: // bottom right corner doesn't move
        newx = newx + geometry.width() - w;
        newy = newy + geometry.height() - h;
        break;
    }
    return QRectF{newx, newy, w, h};
}

// _NET_MOVERESIZE_WINDOW
// note co-ordinates are kwin logical
void X11Window::NETMoveResizeWindow(int flags, qreal x, qreal y, qreal width, qreal height)
{
    int gravity = flags & 0xff;
    int value_mask = 0;
    if (flags & (1 << 8)) {
        value_mask |= XCB_CONFIG_WINDOW_X;
    }
    if (flags & (1 << 9)) {
        value_mask |= XCB_CONFIG_WINDOW_Y;
    }
    if (flags & (1 << 10)) {
        value_mask |= XCB_CONFIG_WINDOW_WIDTH;
    }
    if (flags & (1 << 11)) {
        value_mask |= XCB_CONFIG_WINDOW_HEIGHT;
    }
    configureRequest(value_mask, x, y, width, height, gravity, true);
}

// _GTK_SHOW_WINDOW_MENU
void X11Window::GTKShowWindowMenu(qreal x_root, qreal y_root)
{
    QPoint globalPos(x_root, y_root);
    workspace()->showWindowMenu(QRect(globalPos, globalPos), this);
}

bool X11Window::isMovable() const
{
    if (isUnmanaged()) {
        return false;
    }
    if (!hasNETSupport() && !m_motif.move()) {
        return false;
    }
    if (isFullScreen()) {
        return false;
    }
    if (isSpecialWindow() && !isSplash() && !isToolbar()) { // allow moving of splashscreens :)
        return false;
    }
    if (rules()->checkPosition(invalidPoint) != invalidPoint) { // forced position
        return false;
    }
    return true;
}

bool X11Window::isMovableAcrossScreens() const
{
    if (isUnmanaged()) {
        return false;
    }
    if (!hasNETSupport() && !m_motif.move()) {
        return false;
    }
    if (isSpecialWindow() && !isSplash() && !isToolbar()) { // allow moving of splashscreens :)
        return false;
    }
    if (rules()->checkPosition(invalidPoint) != invalidPoint) { // forced position
        return false;
    }
    return true;
}

bool X11Window::isResizable() const
{
    if (isUnmanaged()) {
        return false;
    }
    if (!hasNETSupport() && !m_motif.resize()) {
        return false;
    }
    if (isFullScreen()) {
        return false;
    }
    if (isSpecialWindow() || isSplash() || isToolbar()) {
        return false;
    }
    if (rules()->checkSize(QSize()).isValid()) { // forced size
        return false;
    }
    const Gravity gravity = interactiveMoveResizeGravity();
    if ((gravity == Gravity::Top || gravity == Gravity::TopLeft || gravity == Gravity::TopRight || gravity == Gravity::Left || gravity == Gravity::BottomLeft) && rules()->checkPosition(invalidPoint) != invalidPoint) {
        return false;
    }

    QSizeF min = minSize();
    QSizeF max = maxSize();
    return min.width() < max.width() || min.height() < max.height();
}

bool X11Window::isMaximizable() const
{
    if (isUnmanaged()) {
        return false;
    }
    if (!isResizable() || isToolbar()) { // SELI isToolbar() ?
        return false;
    }
    if (isAppletPopup()) {
        return false;
    }
    if (rules()->checkMaximize(MaximizeRestore) == MaximizeRestore && rules()->checkMaximize(MaximizeFull) != MaximizeRestore) {
        return true;
    }
    return false;
}

void X11Window::blockGeometryUpdates(bool block)
{
    if (block) {
        ++m_blockGeometryUpdates;
    } else {
        if (--m_blockGeometryUpdates == 0) {
            if (m_lastBufferGeometry != m_bufferGeometry || m_lastFrameGeometry != m_frameGeometry || m_lastClientGeometry != m_clientGeometry) {
                updateServerGeometry();
            }
        }
    }
}

/**
 * Reimplemented to inform the client about the new window position.
 */
void X11Window::moveResizeInternal(const QRectF &rect, MoveResizeMode mode)
{
    // Ok, the shading geometry stuff. Generally, code doesn't care about shaded geometry,
    // simply because there are too many places dealing with geometry. Those places
    // ignore shaded state and use normal geometry, which they usually should get
    // from adjustedSize(). Such geometry comes here, and if the window is shaded,
    // the geometry is used only for client_size, since that one is not used when
    // shading. Then the frame geometry is adjusted for the shaded geometry.
    // This gets more complicated in the case the code does only something like
    // setGeometry( geometry()) - geometry() will return the shaded frame geometry.
    // Such code is wrong and should be changed to handle the case when the window is shaded,
    // for example using X11Window::clientSize()

    if (isUnmanaged()) {
        qCWarning(KWIN_CORE) << "Cannot move or resize unmanaged window" << this;
        return;
    }

    QRectF frameGeometry = Xcb::fromXNative(Xcb::toXNative(rect));
    QRectF clientGeometry = m_clientGeometry;
    if (shade_geometry_change) {
        ; // nothing
    } else if (isShade()) {
        if (frameGeometry.height() == borderTop() + borderBottom()) {
            qCDebug(KWIN_CORE) << "Shaded geometry passed for size:";
        } else {
            clientGeometry = frameRectToClientRect(frameGeometry);
            frameGeometry.setHeight(borderTop() + borderBottom());
        }
    } else {
        clientGeometry = frameRectToClientRect(frameGeometry);
    }
    const QRectF bufferGeometry = frameRectToBufferRect(frameGeometry);

    if (m_bufferGeometry == bufferGeometry && m_clientGeometry == clientGeometry && m_frameGeometry == frameGeometry) {
        return;
    }

    Q_EMIT frameGeometryAboutToChange();

    const QRectF oldBufferGeometry = m_bufferGeometry;
    const QRectF oldFrameGeometry = m_frameGeometry;
    const QRectF oldClientGeometry = m_clientGeometry;
    const Output *oldOutput = m_output;

    m_frameGeometry = frameGeometry;
    m_clientGeometry = clientGeometry;
    m_bufferGeometry = bufferGeometry;
    m_output = workspace()->outputAt(frameGeometry.center());

    updateServerGeometry();
    updateWindowRules(Rules::Position | Rules::Size);

    if (isActive()) {
        workspace()->setActiveOutput(output());
    }
    workspace()->updateStackingOrder();

    if (oldBufferGeometry != m_bufferGeometry) {
        Q_EMIT bufferGeometryChanged(oldBufferGeometry);
    }
    if (oldClientGeometry != m_clientGeometry) {
        Q_EMIT clientGeometryChanged(oldClientGeometry);
    }
    if (oldFrameGeometry != m_frameGeometry) {
        Q_EMIT frameGeometryChanged(oldFrameGeometry);
    }
    if (oldOutput != m_output) {
        Q_EMIT outputChanged();
    }
    Q_EMIT shapeChanged();
}

void X11Window::updateServerGeometry()
{
    if (areGeometryUpdatesBlocked()) {
        return;
    }

    const QRectF oldBufferGeometry = m_lastBufferGeometry;

    // Compute the old client rect, the client geometry is always inside the buffer geometry.
    const QRectF oldClientRect = m_lastClientGeometry.translated(-m_lastBufferGeometry.topLeft());
    const QRectF clientRect = m_clientGeometry.translated(-m_bufferGeometry.topLeft());

    if (oldBufferGeometry.size() != m_bufferGeometry.size() || oldClientRect != clientRect) {
        resizeDecoration();
        // If the client is being interactively resized, then the frame window, the wrapper window,
        // and the client window have correct geometry at this point, so we don't have to configure
        // them again.
        if (m_frame.geometry() != m_bufferGeometry) {
            m_frame.setGeometry(m_bufferGeometry);
        }
        if (!isShade()) {
            if (m_wrapper.geometry() != clientRect) {
                m_wrapper.setGeometry(clientRect);
            }
            if (m_client.geometry() != QRectF(QPointF(0, 0), clientRect.size())) {
                m_client.setGeometry(QRectF(QPointF(0, 0), clientRect.size()));
            }
            // SELI - won't this be too expensive?
            // THOMAS - yes, but gtk+ clients will not resize without ...
            sendSyntheticConfigureNotify();
        }
        updateShape();
    } else {
        m_frame.move(m_bufferGeometry.topLeft());
        sendSyntheticConfigureNotify();
        // Unconditionally move the input window: it won't affect rendering
        m_decoInputExtent.move(pos().toPoint() + inputPos());
    }

    m_lastBufferGeometry = m_bufferGeometry;
    m_lastFrameGeometry = m_frameGeometry;
    m_lastClientGeometry = m_clientGeometry;
}

static bool changeMaximizeRecursion = false;
void X11Window::maximize(MaximizeMode mode)
{
    if (isUnmanaged()) {
        qCWarning(KWIN_CORE) << "Cannot change maximized state of unmanaged window" << this;
        return;
    }

    if (changeMaximizeRecursion) {
        return;
    }

    if (!isResizable() || isToolbar()) { // SELI isToolbar() ?
        return;
    }
    if (!isMaximizable()) {
        return;
    }

    const auto currentQuickTileMode = requestedQuickTileMode();

    QRectF clientArea;
    if (isElectricBorderMaximizing()) {
        clientArea = workspace()->clientArea(MaximizeArea, this, interactiveMoveResizeAnchor());
    } else {
        clientArea = workspace()->clientArea(MaximizeArea, this, moveResizeOutput());
    }

    MaximizeMode old_mode = max_mode;

    mode = rules()->checkMaximize(mode);
    if (max_mode == mode) {
        return;
    }

    blockGeometryUpdates(true);

    // maximing one way and unmaximizing the other way shouldn't happen,
    // so restore first and then maximize the other way
    if ((old_mode == MaximizeVertical && mode == MaximizeHorizontal)
        || (old_mode == MaximizeHorizontal && mode == MaximizeVertical)) {
        maximize(MaximizeRestore); // restore
    }

    Q_EMIT maximizedAboutToChange(mode);
    max_mode = mode;

    // save sizes for restoring, if maximalizing
    QSizeF sz;
    if (isShade()) {
        sz = implicitSize();
    } else {
        sz = size();
    }

    if (quickTileMode() == QuickTileMode(QuickTileFlag::None)) {
        QRectF savedGeometry = geometryRestore();
        if (!(old_mode & MaximizeVertical)) {
            savedGeometry.setTop(y());
            savedGeometry.setHeight(sz.height());
        }
        if (!(old_mode & MaximizeHorizontal)) {
            savedGeometry.setLeft(x());
            savedGeometry.setWidth(sz.width());
        }
        setGeometryRestore(savedGeometry);
    }

    // call into decoration update borders
    if (isDecorated() && decoration()->client() && !(options->borderlessMaximizedWindows() && max_mode == KWin::MaximizeFull)) {
        changeMaximizeRecursion = true;
        const auto c = decoration()->client();
        if ((max_mode & MaximizeVertical) != (old_mode & MaximizeVertical)) {
            Q_EMIT c->maximizedVerticallyChanged(max_mode & MaximizeVertical);
        }
        if ((max_mode & MaximizeHorizontal) != (old_mode & MaximizeHorizontal)) {
            Q_EMIT c->maximizedHorizontallyChanged(max_mode & MaximizeHorizontal);
        }
        if ((max_mode == MaximizeFull) != (old_mode == MaximizeFull)) {
            Q_EMIT c->maximizedChanged(max_mode == MaximizeFull);
        }
        changeMaximizeRecursion = false;
    }

    if (options->borderlessMaximizedWindows()) {
        // triggers a maximize change.
        // The next setNoBorder interation will exit since there's no change but the first recursion pullutes the restore geometry
        changeMaximizeRecursion = true;
        setNoBorder(rules()->checkNoBorder(app_noborder || (m_motif.hasDecoration() && m_motif.noBorder()) || max_mode == MaximizeFull));
        changeMaximizeRecursion = false;
    }

    // Conditional quick tiling exit points
    if (quickTileMode() != QuickTileMode(QuickTileFlag::None)) {
        if (old_mode == MaximizeFull && !clientArea.contains(geometryRestore().center())) {
            // Not restoring on the same screen
            // TODO: The following doesn't work for some reason
            // quick_tile_mode = QuickTileFlag::None; // And exit quick tile mode manually
        } else if ((old_mode == MaximizeVertical && max_mode == MaximizeRestore) || (old_mode == MaximizeFull && max_mode == MaximizeHorizontal)) {
            // Modifying geometry of a tiled window
            updateQuickTileMode(QuickTileFlag::None); // Exit quick tile mode without restoring geometry
        }
    }

    switch (max_mode) {

    case MaximizeVertical: {
        if (old_mode & MaximizeHorizontal) { // actually restoring from MaximizeFull
            if (geometryRestore().width() == 0) {
                // needs placement
                const QSizeF constraintedSize = constrainFrameSize(QSizeF(width() * 2 / 3, clientArea.height()), SizeModeFixedH);
                resize(QSizeF(constraintedSize.width(), clientArea.height()));
                workspace()->placement()->placeSmart(this, clientArea);
            } else {
                moveResize(QRectF(QPointF(geometryRestore().x(), clientArea.top()),
                                  QSize(geometryRestore().width(), clientArea.height())));
            }
        } else {
            moveResize(QRectF(x(), clientArea.top(), width(), clientArea.height()));
        }
        info->setState(NET::MaxVert, NET::Max);
        break;
    }

    case MaximizeHorizontal: {
        if (old_mode & MaximizeVertical) { // actually restoring from MaximizeFull
            if (geometryRestore().height() == 0) {
                // needs placement
                const QSizeF constraintedSize = constrainFrameSize(QSizeF(clientArea.width(), height() * 2 / 3), SizeModeFixedW);
                resize(QSizeF(clientArea.width(), constraintedSize.height()));
                workspace()->placement()->placeSmart(this, clientArea);
            } else {
                moveResize(QRectF(QPoint(clientArea.left(), geometryRestore().y()),
                                  QSize(clientArea.width(), geometryRestore().height())));
            }
        } else {
            moveResize(QRectF(clientArea.left(), y(), clientArea.width(), height()));
        }
        info->setState(NET::MaxHoriz, NET::Max);
        break;
    }

    case MaximizeRestore: {
        QRectF restore = moveResizeGeometry();
        // when only partially maximized, geom_restore may not have the other dimension remembered
        if (old_mode & MaximizeVertical) {
            restore.setTop(geometryRestore().top());
            restore.setBottom(geometryRestore().bottom());
        }
        if (old_mode & MaximizeHorizontal) {
            restore.setLeft(geometryRestore().left());
            restore.setRight(geometryRestore().right());
        }
        if (!restore.isValid()) {
            QSize s = QSize(clientArea.width() * 2 / 3, clientArea.height() * 2 / 3);
            if (geometryRestore().width() > 0) {
                s.setWidth(geometryRestore().width());
            }
            if (geometryRestore().height() > 0) {
                s.setHeight(geometryRestore().height());
            }
            resize(constrainFrameSize(s));
            workspace()->placement()->placeSmart(this, clientArea);
            restore = moveResizeGeometry();
            if (geometryRestore().width() > 0) {
                restore.moveLeft(geometryRestore().x());
            }
            if (geometryRestore().height() > 0) {
                restore.moveTop(geometryRestore().y());
            }
            setGeometryRestore(restore); // relevant for mouse pos calculation, bug #298646
        }

        restore.setSize(constrainFrameSize(restore.size(), SizeModeAny));
        if (isInteractiveMove()) {
            if (!isFullScreen()) {
                const QPointF anchor = interactiveMoveResizeAnchor();
                const QPointF offset = interactiveMoveOffset();
                restore.moveTopLeft(QPointF(anchor.x() - offset.x() * restore.width(),
                                            anchor.y() - offset.y() * restore.height()));
            }
        }

        moveResize(restore);

        info->setState(NET::States(), NET::Max);
        updateQuickTileMode(QuickTileFlag::None);
        break;
    }

    case MaximizeFull: {
        moveResize(clientArea);
        if (options->electricBorderMaximize()) {
            updateQuickTileMode(QuickTileFlag::Maximize);
        } else {
            updateQuickTileMode(QuickTileFlag::None);
        }
        info->setState(NET::Max, NET::Max);
        break;
    }
    default:
        break;
    }

    blockGeometryUpdates(false);
    updateAllowedActions();
    updateWindowRules(Rules::MaximizeVert | Rules::MaximizeHoriz | Rules::Position | Rules::Size);

    if (max_mode != old_mode) {
        Q_EMIT maximizedChanged();
    }
}

void X11Window::setFullScreen(bool set)
{
    set = rules()->checkFullScreen(set);

    const bool wasFullscreen = isFullScreen();
    if (wasFullscreen == set) {
        return;
    }
    if (!isFullScreenable()) {
        return;
    }

    setShade(ShadeNone);

    if (wasFullscreen) {
        workspace()->updateFocusMousePosition(Cursors::self()->mouse()->pos()); // may cause leave event
    } else {
        setFullscreenGeometryRestore(moveResizeGeometry());
    }

    if (set) {
        m_fullscreenMode = FullScreenNormal;
        workspace()->raiseWindow(this);
    } else {
        m_fullscreenMode = FullScreenNone;
    }

    StackingUpdatesBlocker blocker1(workspace());
    X11GeometryUpdatesBlocker blocker2(this);

    // active fullscreens get different layer
    updateLayer();

    info->setState(isFullScreen() ? NET::FullScreen : NET::States(), NET::FullScreen);
    updateDecoration(false, false);

    if (set) {
        if (info->fullscreenMonitors().isSet()) {
            moveResize(fullscreenMonitorsArea(info->fullscreenMonitors()));
        } else {
            moveResize(workspace()->clientArea(FullScreenArea, this, moveResizeOutput()));
        }
    } else {
        Q_ASSERT(!fullscreenGeometryRestore().isNull());
        moveResize(QRectF(fullscreenGeometryRestore().topLeft(), constrainFrameSize(fullscreenGeometryRestore().size())));
    }

    updateWindowRules(Rules::Fullscreen | Rules::Position | Rules::Size);
    updateAllowedActions(false);
    Q_EMIT fullScreenChanged();
}

void X11Window::updateFullscreenMonitors(NETFullscreenMonitors topology)
{
    const int outputCount = workspace()->outputs().count();

    //    qDebug() << "incoming request with top: " << topology.top << " bottom: " << topology.bottom
    //                   << " left: " << topology.left << " right: " << topology.right
    //                   << ", we have: " << nscreens << " screens.";

    if (topology.top >= outputCount || topology.bottom >= outputCount || topology.left >= outputCount || topology.right >= outputCount) {
        qCWarning(KWIN_CORE) << "fullscreenMonitors update failed. request higher than number of screens.";
        return;
    }

    info->setFullscreenMonitors(topology);
    if (isFullScreen()) {
        moveResize(fullscreenMonitorsArea(topology));
    }
}

/**
 * Calculates the bounding rectangle defined by the 4 monitor indices indicating the
 * top, bottom, left, and right edges of the window when the fullscreen state is enabled.
 */
QRect X11Window::fullscreenMonitorsArea(NETFullscreenMonitors requestedTopology) const
{
    QRect total;

    if (auto output = workspace()->xineramaIndexToOutput(requestedTopology.top)) {
        total = total.united(output->geometry());
    }
    if (auto output = workspace()->xineramaIndexToOutput(requestedTopology.bottom)) {
        total = total.united(output->geometry());
    }
    if (auto output = workspace()->xineramaIndexToOutput(requestedTopology.left)) {
        total = total.united(output->geometry());
    }
    if (auto output = workspace()->xineramaIndexToOutput(requestedTopology.right)) {
        total = total.united(output->geometry());
    }

    return total;
}

bool X11Window::doStartInteractiveMoveResize()
{
    if (kwinApp()->operationMode() == Application::OperationModeX11) {
        bool has_grab = false;
        // This reportedly improves smoothness of the moveresize operation,
        // something with Enter/LeaveNotify events, looks like XFree performance problem or something *shrug*
        // (https://lists.kde.org/?t=107302193400001&r=1&w=2)
        QRectF r = workspace()->clientArea(FullArea, this, moveResizeOutput());
        m_moveResizeGrabWindow.create(Xcb::toXNative(r), XCB_WINDOW_CLASS_INPUT_ONLY, 0, nullptr, kwinApp()->x11RootWindow());
        m_moveResizeGrabWindow.map();
        m_moveResizeGrabWindow.raise();
        kwinApp()->updateXTime();
        const xcb_grab_pointer_cookie_t cookie = xcb_grab_pointer_unchecked(kwinApp()->x11Connection(), false, m_moveResizeGrabWindow,
                                                                            XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_POINTER_MOTION | XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_LEAVE_WINDOW,
                                                                            XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC, m_moveResizeGrabWindow, Cursors::self()->mouse()->x11Cursor(cursor()), xTime());
        UniqueCPtr<xcb_grab_pointer_reply_t> pointerGrab(xcb_grab_pointer_reply(kwinApp()->x11Connection(), cookie, nullptr));
        if (pointerGrab && pointerGrab->status == XCB_GRAB_STATUS_SUCCESS) {
            has_grab = true;
        }
        if (!has_grab && grabXKeyboard(frameId())) {
            has_grab = move_resize_has_keyboard_grab = true;
        }
        if (!has_grab) { // at least one grab is necessary in order to be able to finish move/resize
            m_moveResizeGrabWindow.reset();
            return false;
        }
    }
    return true;
}

void X11Window::leaveInteractiveMoveResize()
{
    if (kwinApp()->operationMode() == Application::OperationModeX11) {
        if (move_resize_has_keyboard_grab) {
            ungrabXKeyboard();
        }
        move_resize_has_keyboard_grab = false;
        xcb_ungrab_pointer(kwinApp()->x11Connection(), xTime());
        m_moveResizeGrabWindow.reset();
    }
    Window::leaveInteractiveMoveResize();
}

bool X11Window::isWaitingForInteractiveResizeSync() const
{
    return m_syncRequest.isPending && m_syncRequest.interactiveResize;
}

void X11Window::doInteractiveResizeSync(const QRectF &rect)
{
    setMoveResizeGeometry(rect);

    if (!m_syncRequest.timeout) {
        m_syncRequest.timeout = new QTimer(this);
        connect(m_syncRequest.timeout, &QTimer::timeout, this, &X11Window::handleSyncTimeout);
        m_syncRequest.timeout->setSingleShot(true);
    }

    if (m_syncRequest.counter != XCB_NONE) {
        m_syncRequest.timeout->start(250);
        sendSyncRequest();
    } else {
        // For clients not supporting the XSYNC protocol, we limit the resizes to 30Hz
        // to take pointless load from X11 and the client, the mouse is still moved at
        // full speed and no human can control faster resizes anyway.
        m_syncRequest.isPending = true;
        m_syncRequest.interactiveResize = true;
        m_syncRequest.timeout->start(33);
    }

    const QRectF moveResizeClientGeometry = frameRectToClientRect(moveResizeGeometry());
    const QRectF moveResizeBufferGeometry = frameRectToBufferRect(moveResizeGeometry());

    // According to the Composite extension spec, a window will get a new pixmap allocated each time
    // it is mapped or resized. Given that we redirect frame windows and not client windows, we have
    // to resize the frame window in order to forcefully reallocate offscreen storage. If we don't do
    // this, then we might render partially updated client window. I know, it sucks.
    m_frame.setGeometry(moveResizeBufferGeometry);
    m_wrapper.setGeometry(moveResizeClientGeometry.translated(-moveResizeBufferGeometry.topLeft()));
    m_client.setGeometry(QRectF(QPointF(0, 0), moveResizeClientGeometry.size()));
}

void X11Window::handleSyncTimeout()
{
    if (m_syncRequest.counter == XCB_NONE) { // client w/o XSYNC support. allow the next resize event
        m_syncRequest.isPending = false; // NEVER do this for clients with a valid counter
        m_syncRequest.interactiveResize = false; // (leads to sync request races in some clients)
    }
    performInteractiveResize();
}

NETExtendedStrut X11Window::strut() const
{
    NETExtendedStrut ext = info->extendedStrut();
    NETStrut str = info->strut();
    const QSize displaySize = workspace()->geometry().size();
    if (ext.left_width == 0 && ext.right_width == 0 && ext.top_width == 0 && ext.bottom_width == 0
        && (str.left != 0 || str.right != 0 || str.top != 0 || str.bottom != 0)) {
        // build extended from simple
        if (str.left != 0) {
            ext.left_width = str.left;
            ext.left_start = 0;
            ext.left_end = displaySize.height();
        }
        if (str.right != 0) {
            ext.right_width = str.right;
            ext.right_start = 0;
            ext.right_end = displaySize.height();
        }
        if (str.top != 0) {
            ext.top_width = str.top;
            ext.top_start = 0;
            ext.top_end = displaySize.width();
        }
        if (str.bottom != 0) {
            ext.bottom_width = str.bottom;
            ext.bottom_start = 0;
            ext.bottom_end = displaySize.width();
        }
    }
    return ext;
}

StrutRect X11Window::strutRect(StrutArea area) const
{
    Q_ASSERT(area != StrutAreaAll); // Not valid
    const QSize displaySize = workspace()->geometry().size();
    NETExtendedStrut strutArea = strut();
    switch (area) {
    case StrutAreaTop:
        if (strutArea.top_width != 0) {
            return StrutRect(QRect(
                                 strutArea.top_start, 0,
                                 strutArea.top_end - strutArea.top_start, strutArea.top_width),
                             StrutAreaTop);
        }
        break;
    case StrutAreaRight:
        if (strutArea.right_width != 0) {
            return StrutRect(QRect(
                                 displaySize.width() - strutArea.right_width, strutArea.right_start,
                                 strutArea.right_width, strutArea.right_end - strutArea.right_start),
                             StrutAreaRight);
        }
        break;
    case StrutAreaBottom:
        if (strutArea.bottom_width != 0) {
            return StrutRect(QRect(
                                 strutArea.bottom_start, displaySize.height() - strutArea.bottom_width,
                                 strutArea.bottom_end - strutArea.bottom_start, strutArea.bottom_width),
                             StrutAreaBottom);
        }
        break;
    case StrutAreaLeft:
        if (strutArea.left_width != 0) {
            return StrutRect(QRect(
                                 0, strutArea.left_start,
                                 strutArea.left_width, strutArea.left_end - strutArea.left_start),
                             StrutAreaLeft);
        }
        break;
    default:
        Q_UNREACHABLE(); // Not valid
    }
    return StrutRect(); // Null rect
}

bool X11Window::hasStrut() const
{
    NETExtendedStrut ext = strut();
    if (ext.left_width == 0 && ext.right_width == 0 && ext.top_width == 0 && ext.bottom_width == 0) {
        return false;
    }
    return true;
}

void X11Window::applyWindowRules()
{
    Window::applyWindowRules();
    updateAllowedActions();
    setBlockingCompositing(info->isBlockingCompositing());
}

bool X11Window::supportsWindowRules() const
{
    return !isUnmanaged();
}

void X11Window::updateWindowRules(Rules::Types selection)
{
    if (!isManaged()) { // not fully setup yet
        return;
    }
    Window::updateWindowRules(selection);
}

void X11Window::damageNotifyEvent()
{
    Q_ASSERT(kwinApp()->operationMode() == Application::OperationModeX11);

    if (!readyForPainting()) { // avoid "setReadyForPainting()" function calling overhead
        if (m_syncRequest.counter == XCB_NONE) { // cannot detect complete redraw, consider done now
            setReadyForPainting();
        }
    }

    SurfaceItemX11 *item = static_cast<SurfaceItemX11 *>(surfaceItem());
    if (item) {
        item->processDamage();
    }
}

void X11Window::discardWindowPixmap()
{
    if (auto item = surfaceItem()) {
        item->discardPixmap();
    }
}

void X11Window::updateWindowPixmap()
{
    if (auto item = surfaceItem()) {
        item->updatePixmap();
    }
}

void X11Window::associate()
{
    auto handleMapped = [this]() {
        if (syncRequest().counter == XCB_NONE) { // cannot detect complete redraw, consider done now
            setReadyForPainting();
        }
    };

    if (surface()->isMapped()) {
        handleMapped();
    } else {
        // Queued connection because we want to mark the window ready for painting after
        // the associated surface item has processed the new surface state.
        connect(surface(), &SurfaceInterface::mapped, this, handleMapped, Qt::QueuedConnection);
    }

    m_pendingSurfaceId = 0;
}

QWindow *X11Window::findInternalWindow() const
{
    const QWindowList windows = kwinApp()->topLevelWindows();
    for (QWindow *w : windows) {
        if (w->handle() && w->winId() == window()) {
            return w;
        }
    }
    return nullptr;
}

void X11Window::checkOutput()
{
    setOutput(workspace()->outputAt(frameGeometry().center()));
}

void X11Window::getWmOpaqueRegion()
{
    const auto rects = info->opaqueRegion();
    QRegion new_opaque_region;
    for (const auto &r : rects) {
        new_opaque_region += Xcb::fromXNative(QRect(r.pos.x, r.pos.y, r.size.width, r.size.height)).toRect();
    }
    opaque_region = new_opaque_region;
}

QList<QRectF> X11Window::shapeRegion() const
{
    if (m_shapeRegionIsValid) {
        return m_shapeRegion;
    }

    const QRectF bufferGeometry = this->bufferGeometry();

    if (is_shape) {
        auto cookie = xcb_shape_get_rectangles_unchecked(kwinApp()->x11Connection(), frameId(), XCB_SHAPE_SK_BOUNDING);
        UniqueCPtr<xcb_shape_get_rectangles_reply_t> reply(xcb_shape_get_rectangles_reply(kwinApp()->x11Connection(), cookie, nullptr));
        if (reply) {
            m_shapeRegion.clear();
            const xcb_rectangle_t *rects = xcb_shape_get_rectangles_rectangles(reply.get());
            const int rectCount = xcb_shape_get_rectangles_rectangles_length(reply.get());
            for (int i = 0; i < rectCount; ++i) {
                QRectF region = Xcb::fromXNative(QRect(rects[i].x, rects[i].y, rects[i].width, rects[i].height)).toAlignedRect();
                // make sure the shape is sane (X is async, maybe even XShape is broken)
                region = region.intersected(QRectF(QPointF(0, 0), bufferGeometry.size()));

                m_shapeRegion += region;
            }
        } else {
            m_shapeRegion.clear();
        }
    } else {
        m_shapeRegion = {QRectF(0, 0, bufferGeometry.width(), bufferGeometry.height())};
    }

    m_shapeRegionIsValid = true;
    return m_shapeRegion;
}

void X11Window::discardShapeRegion()
{
    m_shapeRegionIsValid = false;
    m_shapeRegion.clear();
}

Xcb::Property X11Window::fetchWmClientLeader() const
{
    return Xcb::Property(false, window(), atoms->wm_client_leader, XCB_ATOM_WINDOW, 0, 10000);
}

void X11Window::readWmClientLeader(Xcb::Property &prop)
{
    m_wmClientLeader = prop.value<xcb_window_t>(window());
}

void X11Window::getWmClientLeader()
{
    if (isUnmanaged()) {
        return;
    }
    auto prop = fetchWmClientLeader();
    readWmClientLeader(prop);
}

int X11Window::desktopId() const
{
    return m_desktops.isEmpty() ? -1 : m_desktops.last()->x11DesktopNumber();
}

/**
 * Returns sessionId for this window,
 * taken either from its window or from the leader window.
 */
QByteArray X11Window::sessionId() const
{
    QByteArray result = Xcb::StringProperty(window(), atoms->sm_client_id);
    if (result.isEmpty() && m_wmClientLeader && m_wmClientLeader != window()) {
        result = Xcb::StringProperty(m_wmClientLeader, atoms->sm_client_id);
    }
    return result;
}

/**
 * Returns command property for this window,
 * taken either from its window or from the leader window.
 */
QString X11Window::wmCommand()
{
    QByteArray result = Xcb::StringProperty(window(), XCB_ATOM_WM_COMMAND);
    if (result.isEmpty() && m_wmClientLeader && m_wmClientLeader != window()) {
        result = Xcb::StringProperty(m_wmClientLeader, XCB_ATOM_WM_COMMAND);
    }
    result.replace(0, ' ');
    return result;
}

/**
 * Returns client leader window for this client.
 * Returns the client window itself if no leader window is defined.
 */
xcb_window_t X11Window::wmClientLeader() const
{
    if (m_wmClientLeader != XCB_WINDOW_NONE) {
        return m_wmClientLeader;
    }
    return window();
}

void X11Window::getWmClientMachine()
{
    clientMachine()->resolve(window(), wmClientLeader());
}

Xcb::Property X11Window::fetchSkipCloseAnimation() const
{
    return Xcb::Property(false, window(), atoms->kde_skip_close_animation, XCB_ATOM_CARDINAL, 0, 1);
}

void X11Window::readSkipCloseAnimation(Xcb::Property &property)
{
    setSkipCloseAnimation(property.toBool());
}

void X11Window::getSkipCloseAnimation()
{
    Xcb::Property property = fetchSkipCloseAnimation();
    readSkipCloseAnimation(property);
}

//********************************************
// Client
//********************************************

/**
 * Updates the user time (time of last action in the active window).
 * This is called inside  kwin for every action with the window
 * that qualifies for user interaction (clicking on it, activate it
 * externally, etc.).
 */
void X11Window::updateUserTime(xcb_timestamp_t time)
{
    // copied in Group::updateUserTime
    if (time == XCB_TIME_CURRENT_TIME) {
        kwinApp()->updateXTime();
        time = xTime();
    }
    if (time != -1U
        && (m_userTime == XCB_TIME_CURRENT_TIME
            || NET::timestampCompare(time, m_userTime) > 0)) { // time > user_time
        m_userTime = time;
        shade_below = nullptr; // do not hover re-shade a window after it got interaction
    }
    group()->updateUserTime(m_userTime);
}

xcb_timestamp_t X11Window::readUserCreationTime() const
{
    Xcb::Property prop(false, window(), atoms->kde_net_wm_user_creation_time, XCB_ATOM_CARDINAL, 0, 1);
    return prop.value<xcb_timestamp_t>(-1);
}

xcb_timestamp_t X11Window::readUserTimeMapTimestamp(const KStartupInfoId *asn_id, const KStartupInfoData *asn_data,
                                                    bool session) const
{
    xcb_timestamp_t time = info->userTime();
    // qDebug() << "User timestamp, initial:" << time;
    //^^ this deadlocks kwin --replace sometimes.

    // newer ASN timestamp always replaces user timestamp, unless user timestamp is 0
    // helps e.g. with konqy reusing
    if (asn_data != nullptr && time != 0) {
        if (asn_id->timestamp() != 0
            && (time == -1U || NET::timestampCompare(asn_id->timestamp(), time) > 0)) {
            time = asn_id->timestamp();
        }
    }
    qCDebug(KWIN_CORE) << "User timestamp, ASN:" << time;
    if (time == -1U) {
        // The window doesn't have any timestamp.
        // If it's the first window for its application
        // (i.e. there's no other window from the same app),
        // use the _KDE_NET_WM_USER_CREATION_TIME trick.
        // Otherwise, refuse activation of a window
        // from already running application if this application
        // is not the active one (unless focus stealing prevention is turned off).
        X11Window *act = dynamic_cast<X11Window *>(workspace()->mostRecentlyActivatedWindow());
        if (act != nullptr && !belongToSameApplication(act, this, SameApplicationCheck::RelaxedForActive)) {
            bool first_window = true;
            auto sameApplicationActiveHackPredicate = [this](const X11Window *cl) {
                // ignore already existing splashes, toolbars, utilities and menus,
                // as the app may show those before the main window
                return !cl->isSplash() && !cl->isToolbar() && !cl->isUtility() && !cl->isMenu()
                    && cl != this && X11Window::belongToSameApplication(cl, this, SameApplicationCheck::RelaxedForActive);
            };
            if (isTransient()) {
                auto clientMainClients = [this]() {
                    QList<X11Window *> ret;
                    const auto mcs = mainWindows();
                    for (auto mc : mcs) {
                        if (X11Window *c = dynamic_cast<X11Window *>(mc)) {
                            ret << c;
                        }
                    }
                    return ret;
                };
                if (act->hasTransient(this, true)) {
                    ; // is transient for currently active window, even though it's not
                    // the same app (e.g. kcookiejar dialog) -> allow activation
                } else if (groupTransient() && findInList<X11Window, X11Window>(clientMainClients(), sameApplicationActiveHackPredicate) == nullptr) {
                    ; // standalone transient
                } else {
                    first_window = false;
                }
            } else {
#if KWIN_BUILD_X11
                if (workspace()->findClient(sameApplicationActiveHackPredicate)) {
                    first_window = false;
                }
#endif
            }
            // don't refuse if focus stealing prevention is turned off
            if (!first_window && rules()->checkFSP(options->focusStealingPreventionLevel()) > 0) {
                qCDebug(KWIN_CORE) << "User timestamp, already exists:" << 0;
                return 0; // refuse activation
            }
        }
        // Creation time would just mess things up during session startup,
        // as possibly many apps are started up at the same time.
        // If there's no active window yet, no timestamp will be needed,
        // as plain Workspace::allowWindowActivation() will return true
        // in such case. And if there's already active window,
        // it's better not to activate the new one.
        // Unless it was the active window at the time
        // of session saving and there was no user interaction yet,
        // this check will be done in manage().
        if (session) {
            return -1U;
        }
        time = readUserCreationTime();
    }
    qCDebug(KWIN_CORE) << "User timestamp, final:" << this << ":" << time;
    return time;
}

xcb_timestamp_t X11Window::userTime() const
{
    xcb_timestamp_t time = m_userTime;
    if (time == 0) { // doesn't want focus after showing
        return 0;
    }
    Q_ASSERT(group() != nullptr);
    if (time == -1U
        || (group()->userTime() != -1U
            && NET::timestampCompare(group()->userTime(), time) > 0)) {
        time = group()->userTime();
    }
    return time;
}

void X11Window::doSetActive()
{
    updateUrgency(); // demand attention again if it's still urgent
    info->setState(isActive() ? NET::Focused : NET::States(), NET::Focused);
}

void X11Window::startupIdChanged()
{
    KStartupInfoId asn_id;
    KStartupInfoData asn_data;
    bool asn_valid = workspace()->checkStartupNotification(window(), asn_id, asn_data);
    if (!asn_valid) {
        return;
    }
    // If the ASN contains desktop, move it to the desktop, otherwise move it to the current
    // desktop (since the new ASN should make the window act like if it's a new application
    // launched). However don't affect the window's desktop if it's set to be on all desktops.

    if (asn_data.desktop() != 0 && !isOnAllDesktops()) {
        if (asn_data.desktop() == -1) {
            workspace()->sendWindowToDesktops(this, {}, true);
        } else {
            if (VirtualDesktop *desktop = VirtualDesktopManager::self()->desktopForX11Id(asn_data.desktop())) {
                workspace()->sendWindowToDesktops(this, {desktop}, true);
            }
        }
    }

    if (asn_data.xinerama() != -1) {
        Output *output = workspace()->xineramaIndexToOutput(asn_data.xinerama());
        if (output) {
            workspace()->sendWindowToOutput(this, output);
        }
    }
    const xcb_timestamp_t timestamp = asn_id.timestamp();
    if (timestamp != 0) {
        bool activate = allowWindowActivation(timestamp);
        if (activate) {
            workspace()->activateWindow(this);
        } else {
            demandAttention();
        }
    }
}

void X11Window::updateUrgency()
{
    if (info->urgency()) {
        demandAttention();
    }
}

namespace FSP
{
enum Level {
    None = 0,
    Low,
    Medium,
    High,
    Extreme,
};
}

// focus_in -> the window got FocusIn event
bool X11Window::allowWindowActivation(xcb_timestamp_t time, bool focus_in)
{
    auto window = this;
    // options->focusStealingPreventionLevel :
    // 0 - none    - old KWin behaviour, new windows always get focus
    // 1 - low     - focus stealing prevention is applied normally, when unsure, activation is allowed
    // 2 - normal  - focus stealing prevention is applied normally, when unsure, activation is not allowed,
    //              this is the default
    // 3 - high    - new window gets focus only if it belongs to the active application,
    //              or when no window is currently active
    // 4 - extreme - no window gets focus without user intervention
    if (time == -1U) {
        time = window->userTime();
    }
    const FSP::Level level = (FSP::Level)window->rules()->checkFSP(options->focusStealingPreventionLevel());
    if (workspace()->sessionManager()->state() == SessionState::Saving && level <= FSP::Medium) { // <= normal
        return true;
    }
    Window *ac = workspace()->mostRecentlyActivatedWindow();
    if (focus_in) {
        if (workspace()->inShouldGetFocus(window)) {
            return true; // FocusIn was result of KWin's action
        }
        // Before getting FocusIn, the active Client already
        // got FocusOut, and therefore got deactivated.
        ac = workspace()->lastActiveWindow();
    }
    if (time == 0) { // explicitly asked not to get focus
        if (!window->rules()->checkAcceptFocus(false)) {
            return false;
        }
    }
    const FSP::Level protection = (FSP::Level)(ac ? ac->rules()->checkFPP(2) : FSP::None);

    // stealing is unconditionally allowed (NETWM behavior)
    if (level == FSP::None || protection == FSP::None) {
        return true;
    }

    // The active window "grabs" the focus or stealing is generally forbidden
    if (level == FSP::Extreme || protection == FSP::Extreme) {
        return false;
    }

    // No active window, it's ok to pass focus
    // NOTICE that extreme protection needs to be handled before to allow protection on unmanged windows
    if (ac == nullptr || ac->isDesktop()) {
        qCDebug(KWIN_CORE) << "Activation: No window active, allowing";
        return true; // no active window -> always allow
    }

    // TODO window urgency  -> return true?

    // Unconditionally allow intra-window passing around for lower stealing protections
    // unless the active window has High interest
    if (Window::belongToSameApplication(window, ac, Window::SameApplicationCheck::RelaxedForActive) && protection < FSP::High) {
        qCDebug(KWIN_CORE) << "Activation: Belongs to active application";
        return true;
    }

    // High FPS, not intr-window change. Only allow if the active window has only minor interest
    if (level > FSP::Medium && protection > FSP::Low) {
        return false;
    }

    if (time == -1U) { // no time known
        qCDebug(KWIN_CORE) << "Activation: No timestamp at all";
        // Only allow for Low protection unless active window has High interest in focus
        if (level < FSP::Medium && protection < FSP::High) {
            return true;
        }
        // no timestamp at all, don't activate - because there's also creation timestamp
        // done on CreateNotify, this case should happen only in case application
        // maps again already used window, i.e. this won't happen after app startup
        return false;
    }

    // Low or medium FSP, usertime comparism is possible
    const xcb_timestamp_t user_time = ac->userTime();
    qCDebug(KWIN_CORE) << "Activation, compared:" << window << ":" << time << ":" << user_time
                       << ":" << (NET::timestampCompare(time, user_time) >= 0);
    return NET::timestampCompare(time, user_time) >= 0; // time >= user_time
}

void X11Window::restackWindow(xcb_window_t above, int detail, NET::RequestSource src, xcb_timestamp_t timestamp, bool send_event)
{
    X11Window *other = nullptr;
    if (detail == XCB_STACK_MODE_OPPOSITE) {
        other = workspace()->findClient(Predicate::WindowMatch, above);
        if (!other) {
            workspace()->raiseOrLowerWindow(this);
            return;
        }
        auto it = workspace()->stackingOrder().constBegin(),
             end = workspace()->stackingOrder().constEnd();
        while (it != end) {
            if (*it == this) {
                detail = XCB_STACK_MODE_ABOVE;
                break;
            } else if (*it == other) {
                detail = XCB_STACK_MODE_BELOW;
                break;
            }
            ++it;
        }
    } else if (detail == XCB_STACK_MODE_TOP_IF) {
        other = workspace()->findClient(Predicate::WindowMatch, above);
        if (other && other->frameGeometry().intersects(frameGeometry())) {
            workspace()->raiseWindowRequest(this, src, timestamp);
        }
        return;
    } else if (detail == XCB_STACK_MODE_BOTTOM_IF) {
        other = workspace()->findClient(Predicate::WindowMatch, above);
        if (other && other->frameGeometry().intersects(frameGeometry())) {
            workspace()->lowerWindowRequest(this, src, timestamp);
        }
        return;
    }

    if (!other) {
        other = workspace()->findClient(Predicate::WindowMatch, above);
    }

    if (other && detail == XCB_STACK_MODE_ABOVE) {
        auto it = workspace()->stackingOrder().constEnd(),
             begin = workspace()->stackingOrder().constBegin();
        while (--it != begin) {

            if (*it == other) { // the other one is top on stack
                it = begin; // invalidate
                src = NET::FromTool; // force
                break;
            }
            X11Window *window = qobject_cast<X11Window *>(*it);

            if (!window || !((*it)->isNormalWindow() && window->isShown() && (*it)->isOnCurrentDesktop() && (*it)->isOnCurrentActivity() && (*it)->isOnOutput(output()))) {
                continue; // irrelevant windows
            }

            if (*(it - 1) == other) {
                break; // "it" is the one above the target one, stack below "it"
            }
        }

        if (it != begin && (*(it - 1) == other)) {
            other = qobject_cast<X11Window *>(*it);
        } else {
            other = nullptr;
        }
    }

    if (other) {
        workspace()->restack(this, other);
    } else if (detail == XCB_STACK_MODE_BELOW) {
        workspace()->lowerWindowRequest(this, src, timestamp);
    } else if (detail == XCB_STACK_MODE_ABOVE) {
        workspace()->raiseWindowRequest(this, src, timestamp);
    }

    if (send_event) {
        sendSyntheticConfigureNotify();
    }
}

bool X11Window::belongsToDesktop() const
{
    const auto members = group()->members();
    for (const X11Window *window : members) {
        if (window->isDesktop()) {
            return true;
        }
    }
    return false;
}

void X11Window::setShortcutInternal()
{
    updateCaption();
#if 0
    workspace()->windowShortcutUpdated(this);
#else
    // Workaround for kwin<->kglobalaccel deadlock, when KWin has X grab and the kded
    // kglobalaccel module tries to create the key grab. KWin should preferably grab
    // they keys itself anyway :(.
    QTimer::singleShot(0, this, std::bind(&Workspace::windowShortcutUpdated, workspace(), this));
#endif
}

} // namespace

#include "moc_x11window.cpp"
