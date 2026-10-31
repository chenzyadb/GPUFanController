#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

struct GLFWwindow;

#if defined(_WIN32) && !defined(OPENGLGUI_STATIC)
#if defined(OPENGLGUI_BUILD)
#define OPENGLGUI_API __declspec(dllexport)
#else
#define OPENGLGUI_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) && !defined(OPENGLGUI_STATIC)
#define OPENGLGUI_API __attribute__((visibility("default")))
#else
#define OPENGLGUI_API
#endif

namespace OpenGLGUI
{
    class MainWindow;
    class Widget;

    /** Registers and resolves application-wide TrueType font families. */
    class OPENGLGUI_API FontManager
    {
        public:
            /** Registers a font file under its embedded family name and returns that name. */
            [[nodiscard]] static std::string addFont(std::string_view filePath);

            /** Registers a font file under an explicit application family name. */
            [[nodiscard]] static std::string addFont(std::string_view familyName, std::string_view filePath);

            /** Selects the default family used by newly-created text widgets. */
            static void setDefaultFontFamily(std::string_view familyName);

            /** Restores the platform system font as the application-wide default. */
            static void useSystemFont();

            /** Returns the current application-wide default family name. */
            [[nodiscard]] static std::string getDefaultFontFamily();

            /** Returns the process-wide manager facade for code preferring an object handle. */
            [[nodiscard]] static FontManager& instance() noexcept;
            [[nodiscard]] static FontManager& getInstance() noexcept;

        private:
            FontManager() = default;
    };

    /**
     * Returns the widest explicit UTF-8 line's advance width in dp, without wrapping or padding.
     * Uses a registered FontManager family, a weight in [100, 900], and a positive finite fontSize
     * in dp. Includes kerning, fallback glyphs, and synthetic weight, as in text widgets. Tabs
     * expand to four spaces; carriage returns and other control characters are ignored. Empty
     * text returns zero. Invalid family, weight, or size throws std::invalid_argument.
     * This is a layout advance, not a rasterized glyph-ink bounding box; no window is required.
     */
    [[nodiscard]] OPENGLGUI_API float GetTextWidth(
        std::string_view fontFamily, unsigned int fontWeight, float fontSize, std::string_view text);

    /**
     * Returns the single-line ascent-to-descent height in dp, excluding line gap and padding.
     * Uses the same registered family, weight, size, and validation rules as GetTextWidth.
     * The current renderer normalizes this height to fontSize; synthetic weight does not change
     * vertical metrics. No window or OpenGL context is required.
     */
    [[nodiscard]] OPENGLGUI_API float GetTextHeight(
        std::string_view fontFamily, unsigned int fontWeight, float fontSize);

    namespace Internal
    {
        class RenderPayload;
        struct QueuedEvent;
        struct TrueTypeFontData;
        struct WidgetRenderData;

        /** Advances all active animations on the Application thread. */
        void processAnimations(std::chrono::steady_clock::time_point now);

        /** Silently clears animations when the owning Application is retired. */
        void clearAnimations() noexcept;

        /** Submits each window dirtied during the current animation batch. */
        void endAnimationBatch();
    } // namespace Internal

    using WindowId = GLFWwindow*;          ///< Non-owning cross-platform GLFW window identifier.
    using NativeWidgetId = std::uintptr_t; ///< Platform-native child-component handle encoded without
                                           ///< exposing platform headers.

    using Callback = std::function<void()>; ///< Callback without event data.

    /** Identifies the mouse button that completed a Widget click. */
    enum class MouseButton {
        LEFT,  ///< Primary mouse button.
        MID,   ///< Middle mouse button.
        RIGHT, ///< Secondary mouse button.
    };

    /** Provides an RTTI-free type-erased view over a copyable event payload. */
    class OPENGLGUI_API EventData
    {
        private:
            /** Copies a concrete payload into type-erased queue-owned storage. */
            using PayloadCopier = std::shared_ptr<const void> (*)(const void*);

            /**
             * Copies one concrete payload for deferred delivery by the EventBus queue.
             */
            template <typename stored_type>
            static std::shared_ptr<const void> copyPayload(const void* data)
            {
                return std::make_shared<stored_type>(*static_cast<const stored_type*>(data));
            }

            /**
             * Selects the copier for a normalized payload type and enforces queue
             * ownership requirements.
             */
            template <typename value_type>
            static constexpr PayloadCopier payloadCopierFor() noexcept
            {
                using stored_type = std::remove_cv_t<std::remove_reference_t<value_type>>;
                static_assert(std::is_copy_constructible_v<stored_type>,
                    "EventData payloads must be copy constructible for queued delivery");

                return &copyPayload<stored_type>;
            }

            /**
             * Hashes one normalized compiler type signature into a process-independent
             * ID.
             */
            template <typename stored_type>
            static constexpr std::size_t typeIdForNormalized() noexcept
            {
                static_assert(std::is_object_v<stored_type>, "EventData payload types must be object types");
#if defined(_MSC_VER)
                constexpr std::string_view signature = __FUNCSIG__;
#else
                constexpr std::string_view signature = __PRETTY_FUNCTION__;
#endif
                const std::size_t offsetBasis = sizeof(std::size_t) == 8
                    ? static_cast<std::size_t>(1469598103934665603ULL)
                    : static_cast<std::size_t>(2166136261U); // Width-specific FNV-1a offset basis.
                const std::size_t prime = sizeof(std::size_t) == 8
                    ? static_cast<std::size_t>(1099511628211ULL)
                    : static_cast<std::size_t>(16777619U); // Width-specific FNV-1a prime.
                std::size_t hash = offsetBasis;

                for (const char character : signature) {
                    hash ^= static_cast<unsigned char>(character);
                    hash *= prime;
                }

                return hash == 0 ? 1 : hash;
            }

        public:
            /** Creates an empty payload for events without associated data. */
            constexpr EventData() noexcept = default;

            /** Creates an empty payload from a null pointer literal. */
            constexpr EventData(std::nullptr_t) noexcept { }

            /**
             * Captures one payload value for copying into the EventBus delivery queue.
             */
            template <typename value_type,
                typename =
                    std::enable_if_t<!std::is_same_v<std::remove_cv_t<std::remove_reference_t<value_type>>, EventData>>>
            EventData(const value_type& value) noexcept
                : data_(static_cast<const void*>(std::addressof(value))), typeId_(TypeId<value_type>()),
                  payloadCopier_(payloadCopierFor<value_type>())
            {
                static_assert(std::is_object_v<std::remove_cv_t<std::remove_reference_t<value_type>>>,
                    "EventData payloads must be object types");
            }

            /** Returns a stable compile-time ID for one payload type without RTTI. */
            template <typename value_type>
            static constexpr std::size_t TypeId() noexcept
            {
                using stored_type = std::remove_cv_t<std::remove_reference_t<value_type>>;

                return typeIdForNormalized<stored_type>();
            }

            /** Returns the stored payload when its type matches, or nullptr otherwise. */
            template <typename value_type>
            [[nodiscard]] const std::remove_cv_t<std::remove_reference_t<value_type>>* get() const noexcept
            {
                using stored_type = std::remove_cv_t<std::remove_reference_t<value_type>>;
                static_assert(std::is_object_v<stored_type>, "EventData payload types must be object types");

                if (typeId_ != TypeId<stored_type>()) {
                    return nullptr;
                }

                return static_cast<const stored_type*>(data_);
            }

            /** Returns whether the payload contains a value of the requested type. */
            template <typename value_type>
            [[nodiscard]] bool is() const noexcept
            {
                using stored_type = std::remove_cv_t<std::remove_reference_t<value_type>>;

                return data_ != nullptr && typeId_ == TypeId<stored_type>();
            }

            /** Returns whether this event carries no payload. */
            [[nodiscard]] constexpr bool empty() const noexcept
            {
                return data_ == nullptr;
            }

        private:
            friend struct Internal::QueuedEvent;

            const void* data_{};            ///< Payload view valid while the receiving callback is running.
            std::size_t typeId_{};          ///< Compile-time payload type record used to validate
                                            ///< get<T>().
            PayloadCopier payloadCopier_{}; ///< Type-specific copy operation used when
                                            ///< an event enters the queue.
    };

    /** Descriptive aliases for code that refers to an event message or payload. */
    using EventMessage = EventData;
    using EventPayload = EventData;

    /** Maintains sender/event/receiver subscriptions on the Application thread. */
    class OPENGLGUI_API EventBus
    {
        public:
            /**
             * Adds one receiver for a sender object and integer event identifier on the
             * Application thread.
             */
            static void Subscribe(
                void* postObject, int eventId, void* receiveObject, std::function<void(const EventData&)> receiver);

            /**
             * Synchronously drains matching events; subscribed posts made during delivery
             * enter the next FIFO batch. Events without subscribers at Post time are discarded
             * without copying the payload, allocating, or queuing for future subscribers.
             * Active callback lists remain stable under subscription changes; callback
             * instances are retained across posts rather than copied for each invocation.
             */
            static void Post(void* postObject, int eventId, const EventData& message = {});

            /**
             * Removes all matching subscriptions for a sender, event, and receiver on
             * the Application thread.
             */
            static void Unsubscribe(void* postObject, int eventId, void* receiveObject);

            /**
             * Removes every subscription emitted by one sender object on the Application
             * thread.
             */
            static void UnsubscribeSender(void* postObject);
    };

    /**
     * Describes a component position in dp or a top-level position in native
     * screen coordinates.
     */
    struct OPENGLGUI_API Position
    {
            int left{}; ///< Horizontal component offset in dp or top-level native screen
                        ///< coordinate.
            int top{};  ///< Vertical component offset in dp or top-level native screen
                        ///< coordinate.

            /** Creates a zero-origin position. */
            constexpr Position() noexcept = default;

            /** Creates a position from horizontal and vertical offsets. */
            constexpr Position(int leftValue, int topValue) noexcept : left(leftValue), top(topValue) { }
    };

    /** Describes a window or component size in density-independent pixels. */
    struct OPENGLGUI_API Size
    {
            int width{};  ///< Width of the client or component area in dp.
            int height{}; ///< Height of the client or component area in dp.

            /** Creates an empty size. */
            constexpr Size() noexcept = default;

            /** Creates a size from its width and height. */
            constexpr Size(int widthValue, int heightValue) noexcept : width(widthValue), height(heightValue) { }
    };

    /** Stores layout padding in density-independent pixels. */
    struct OPENGLGUI_API Padding
    {
            int start{};  ///< Leading padding on the layout's horizontal start edge.
            int end{};    ///< Trailing padding on the layout's horizontal end edge.
            int top{};    ///< Top padding.
            int bottom{}; ///< Bottom padding.

            /** Creates empty padding on every edge. */
            constexpr Padding() noexcept = default;

            /** Creates padding from start, end, top, and bottom values in dp. */
            constexpr Padding(int startValue, int endValue, int topValue, int bottomValue) noexcept
                : start(startValue), end(endValue), top(topValue), bottom(bottomValue)
            { }
    };

    /** Bit flags used to align layout children along either axis. */
    inline constexpr unsigned int ALIGN_LEFT = 1U << 0U;              ///< Aligns children to the left edge.
    inline constexpr unsigned int ALIGN_HORIZONTAL_CENTER = 1U << 1U; ///< Centers children horizontally.
    inline constexpr unsigned int ALIGN_RIGHT = 1U << 2U;             ///< Aligns children to the right edge.
    inline constexpr unsigned int ALIGN_TOP = 1U << 3U;               ///< Aligns children to the top edge.
    inline constexpr unsigned int ALIGN_VERTICAL_CENTER = 1U << 4U;   ///< Centers children vertically.
    inline constexpr unsigned int ALIGN_BOTTOM = 1U << 5U;            ///< Aligns children to the bottom edge.
    inline constexpr unsigned int ALIGN_CENTER =
        ALIGN_VERTICAL_CENTER | ALIGN_HORIZONTAL_CENTER; ///< Centers content on both axes.

    /** Stores a normalized RGBA color. */
    struct OPENGLGUI_API Color
    {
            float red{};   ///< Red channel in the range [0, 1].
            float green{}; ///< Green channel in the range [0, 1].
            float blue{};  ///< Blue channel in the range [0, 1].
            float alpha{}; ///< Alpha channel in the range [0, 1].

            static const Color WHITE;   ///< Opaque white.
            static const Color BLACK;   ///< Opaque black.
            static const Color GRAY;    ///< Opaque middle gray.
            static const Color BLUE;    ///< Opaque blue.
            static const Color CYAN;    ///< Opaque cyan.
            static const Color TEAL;    ///< Opaque teal.
            static const Color GREEN;   ///< Opaque green.
            static const Color RED;     ///< Opaque red.
            static const Color PURPLE;  ///< Opaque purple.
            static const Color MAGENTA; ///< Opaque magenta.
            static const Color YELLOW;  ///< Opaque yellow.
            static const Color ORANGE;  ///< Opaque orange.

            /** Creates a transparent black color. */
            Color() noexcept = default;

            /**
             * Creates a color from 8-bit-style RGBA channels in the inclusive range [0,
             * 255].
             */
            Color(unsigned int red, unsigned int green, unsigned int blue, unsigned int alpha = 255U);

            /** Creates a color from #RGB, #RGBA, #RRGGBB, or #RRGGBBAA notation. */
            explicit Color(std::string_view hexadecimalColor);
    };

    /** Owns the process-wide GUI lifecycle and main-thread event pump. */
    class OPENGLGUI_API Application
    {
        public:
            /** Registers the single application instance for the current process. */
            Application();

            /** Unregisters this application after its windows have been destroyed. */
            ~Application();

            Application(const Application&) = delete;
            Application& operator=(const Application&) = delete;
            Application(Application&&) = delete;
            Application& operator=(Application&&) = delete;

            /**
             * Runs the blocking application event loop until every native window closes.
             */
            int exec();

            /** Queues asynchronous FIFO work for the Application thread. */
            static void RunOnMainThread(Callback callback);

            /** Queues asynchronous FIFO work for the process-wide IO thread. */
            static void RunOnIOThread(Callback callback);
    };

    /**
     * Provides a main-thread, refresh-paced timeline for custom visual animations.
     */
    enum class EasingCurve {
        LINEAR,      ///< Keeps the timeline progress unchanged.
        IN_QUAD,     ///< Starts slowly and accelerates quadratically.
        OUT_QUAD,    ///< Starts quickly and decelerates quadratically.
        IN_OUT_QUAD, ///< Applies quadratic easing at both ends of the timeline.
    };

    class OPENGLGUI_API Animation
    {
        public:
            /** Nested alias for callers that scope the curve through Animation. */
            using EasingCurve = OpenGLGUI::EasingCurve;

            /** Identifies lifecycle events emitted by an Animation sender. */
            enum class EventId : int {
                ON_START = 1,    ///< Animation entered the running state.
                ON_STOP = 2,     ///< Animation was explicitly stopped before completion.
                ON_FINISHED = 3, ///< Animation reached its end progress naturally.
            };

            static constexpr int ON_START = static_cast<int>(EventId::ON_START); ///< Integer alias for the start event.
            static constexpr int ON_STOP = static_cast<int>(EventId::ON_STOP);   ///< Integer alias for the stop event.
            static constexpr int ON_FINISHED =
                static_cast<int>(EventId::ON_FINISHED); ///< Integer alias for the finished event.

            /** Releases the animation and removes it from the main-thread scheduler. */
            virtual ~Animation();

            Animation(const Animation&) = delete;
            Animation& operator=(const Animation&) = delete;
            Animation(Animation&&) = delete;
            Animation& operator=(Animation&&) = delete;

            /**
             * Starts this animation on the Application construction thread, reusing
             * the duration stored by the previous start(duration) call or the default
             * 300 milliseconds.
             */
            Animation& start();

            /**
             * Sets the timeline duration and starts this animation on the Application
             * construction thread.
             */
            Animation& start(std::chrono::milliseconds duration);

            /**
             * Sets the timeline duration in integer milliseconds and starts this
             * animation.
             */
            Animation& start(int durationMilliseconds);

            /** Stops this animation on the Application construction thread. */
            Animation& stop();

            /**
             * Temporarily freezes a running timeline without losing its progress;
             * the scheduler skips the animation while paused.
             */
            Animation& pause();

            /** Resumes a paused timeline from the progress reached when pause() ran. */
            Animation& resume();

            /** Selects whether the scheduler deletes this animation after ON_FINISHED. */
            Animation& deleteAfterFinished(bool enabled) noexcept;

            /** Selects the easing curve applied to progress before update() callbacks. */
            Animation& easingCurve(EasingCurve curve) noexcept;

            /** Returns the configured easing curve. */
            [[nodiscard]] EasingCurve getEasingCurve() const noexcept;

            /** Returns whether this animation is currently scheduled for updates. */
            [[nodiscard]] bool isRunning() const noexcept;

            /**
             * Returns whether this running animation is temporarily frozen by pause().
             */
            [[nodiscard]] bool isPaused() const noexcept;

        protected:
            /** Creates a timeline with a default 300 millisecond duration. */
            Animation() noexcept;

            /**
             * Applies one normalized progress value in the inclusive range [0, 1].
             * The scheduler applies the selected easing curve before this callback.
             */
            virtual void update(float progress);

            /**
             * Optional semantic update hook used by derived classes that prefer
             * onUpdate().
             */
            virtual void onUpdate(float progress);

            /** Returns the configured timeline duration for derived animation logic. */
            [[nodiscard]] std::chrono::milliseconds duration() const noexcept;

            /** Clamps a callback progress value to the inclusive range [0, 1]. */
            [[nodiscard]] static float clampProgress(float progress) noexcept;

            /** Interpolates a floating-point value using a normalized progress value. */
            [[nodiscard]] static float interpolate(float start, float end, float progress) noexcept;

            /**
             * Interpolates an integer value using a normalized progress value and
             * rounds to the nearest representable coordinate.
             */
            [[nodiscard]] static int interpolate(int start, int end, float progress) noexcept;

            /** Interpolates each normalized RGBA channel between two colors. */
            [[nodiscard]] static Color interpolate(const Color& start, const Color& end, float progress) noexcept;

        private:
            friend void Internal::processAnimations(std::chrono::steady_clock::time_point now);
            friend void Internal::clearAnimations() noexcept;

            /** Advances this timeline once from the main-thread scheduler. */
            void advance(std::chrono::steady_clock::time_point now);

            /**
             * Deactivates this timeline silently while the Application is shutting down.
             */
            void cancelFromApplicationShutdown() noexcept;

            std::chrono::milliseconds duration_{};                     ///< Total linear timeline duration.
            std::chrono::steady_clock::time_point startedAt_{};        ///< Monotonic start timestamp while running.
            std::chrono::steady_clock::duration elapsedBeforePause_{}; ///< Frozen progress accumulated before pause().
            bool running_{};                               ///< Whether this animation is registered for main-thread
                                                           ///< updates.
            bool paused_{};                                ///< Whether the running timeline is temporarily frozen.
            bool deleteAfterFinished_{};                   ///< Whether natural completion transfers
                                                           ///< deletion to the scheduler.
            EasingCurve easingCurve_{EasingCurve::LINEAR}; ///< Progress interpolation curve.
    };

    /**
     * Protected-constructor base class for parent-owned visual components in a
     * window widget tree.
     */
    class OPENGLGUI_API Widget
    {
        public:
            /** Identifies the basic events emitted by a Widget sender. */
            enum class EventId : int {
                ON_ENTERED = 1,     ///< Pointer entered this component.
                ON_EXITED = 2,      ///< Pointer left this component.
                ON_DESTROYED = 4,   ///< Component is being destroyed.
                ON_RESIZED = 5,     ///< Component dimensions changed.
                ON_CLICKED = 6,     ///< Component was clicked; EventData contains a transient
                                    ///< MouseButton.
                ON_KEY_PRESSED = 7, ///< A key was pressed while this component had focus.
                ON_REPAINTED = 8,   ///< Component repaint was submitted.
                ON_GOT_FOCUS = 9,   ///< Component acquired keyboard focus.
                ON_LOST_FOCUS = 10, ///< Component released keyboard focus.
            };

            static constexpr int ON_ENTERED =
                static_cast<int>(EventId::ON_ENTERED); ///< Integer alias for EventId::ON_ENTERED.
            static constexpr int ON_EXITED =
                static_cast<int>(EventId::ON_EXITED); ///< Integer alias for EventId::ON_EXITED.
            static constexpr int ON_DESTROYED =
                static_cast<int>(EventId::ON_DESTROYED); ///< Integer alias for EventId::ON_DESTROYED.
            static constexpr int ON_RESIZED =
                static_cast<int>(EventId::ON_RESIZED); ///< Integer alias for EventId::ON_RESIZED.
            static constexpr int ON_CLICKED =
                static_cast<int>(EventId::ON_CLICKED); ///< Integer alias for EventId::ON_CLICKED.
            static constexpr int ON_KEY_PRESSED =
                static_cast<int>(EventId::ON_KEY_PRESSED); ///< Integer alias for EventId::ON_KEY_PRESSED.
            static constexpr int ON_REPAINTED =
                static_cast<int>(EventId::ON_REPAINTED); ///< Integer alias for EventId::ON_REPAINTED.
            static constexpr int ON_GOT_FOCUS =
                static_cast<int>(EventId::ON_GOT_FOCUS); ///< Integer alias for EventId::ON_GOT_FOCUS.
            static constexpr int ON_LOST_FOCUS =
                static_cast<int>(EventId::ON_LOST_FOCUS); ///< Integer alias for EventId::ON_LOST_FOCUS.

            /**
             * Immediately releases owned children, detaches from the parent, and clears window references.
             * Emits ON_DESTROYED after derived destructors run; observers must not access derived state.
             * Use destroy() instead of direct deletion during event dispatch.
             */
            virtual ~Widget() = 0;

            Widget(const Widget&) = delete;
            Widget& operator=(const Widget&) = delete;
            Widget(Widget&&) = delete;
            Widget& operator=(Widget&&) = delete;

            /**
             * Sets the position relative to the parent widget in density-independent
             * pixels.
             */
            Widget& place(Position position) noexcept;

            /** Sets the component dimensions in density-independent pixels. */
            virtual Widget& size(Size dimensions);

            /** Controls whether both dimensions fill the direct parent or root window. */
            Widget& fillMaxSize(bool enabled);

            /** Controls whether the width fills the direct parent or root window width. */
            Widget& fillMaxWidth(bool enabled);

            /** Controls whether the height fills the direct parent or root window height. */
            Widget& fillMaxHeight(bool enabled);

            /** Limits the maximum dimensions applied by the active fill axes. */
            Widget& maxSize(Size dimensions);

            /** Limits the minimum dimensions applied by the active fill axes. */
            Widget& minSize(Size dimensions);

            /**
             * Controls whether this component and its descendants participate in
             * rendering.
             */
            Widget& visible(bool enabled) noexcept;

            /** Sets the component background to a normalized RGBA color. */
            Widget& backgroundColor(const Color& color);

            /** Sets the component corner radius in density-independent pixels. */
            Widget& borderRadius(float radius);

            /**
             * Sets the component border width in density-independent pixels; zero
             * disables the border.
             */
            Widget& borderWidth(float dp);

            /** Sets the component border to a normalized RGBA color. */
            Widget& borderColor(const Color& color);

            /**
             * Sets the component shadow width in density-independent pixels; zero
             * disables the shadow.
             */
            Widget& shadowWidth(float dp);

            /** Sets the component shadow to a normalized RGBA color. */
            Widget& shadowColor(const Color& color);

            /**
             * Sets the Gaussian blur radius in density-independent pixels applied to
             * everything inside the component bounds, including its own background and
             * content; zero skips blur entirely. Semi-transparent components blur the
             * backdrop showing through them, producing a frosted-glass overlay.
             */
            Widget& blurRadius(float radius);

            /** Sets the component opacity in the inclusive range [0, 1]. */
            Widget& alpha(float value);

            /** Controls whether pressing this component can assign keyboard focus. */
            Widget& focusable(bool enabled) noexcept;

            /**
             * Requests keyboard focus when this component is focusable and attached to a
             * window.
             */
            Widget& setFocus() noexcept;

            /** Clears keyboard focus when this component is the current window target. */
            Widget& clearFocus() noexcept;

            /**
             * Asynchronously submits the complete owning window tree, including changes
             * to other widgets. Posts ON_REPAINTED on this widget, each ancestor, and its window.
             * Rapid commits may coalesce; return does not imply presentation.
             */
            Widget& commit();

            /**
             * Queues cleanup and deletion at the end of the Application main-thread FIFO.
             * Call only on the GUI thread for heap-allocated widgets. Repeated requests
             * are ignored; resources and parent links remain intact until execution.
             * Requests made before exec() wait for event processing. Application teardown
             * also completes pending deletions. Owner or direct deletion cancels the request.
             */
            void destroy() noexcept;

            /**
             * Moves this widget one level above its next sibling and immediately
             * repaints.
             */
            Widget& raise();

            /**
             * Moves this widget one level below its previous sibling and immediately
             * repaints.
             */
            Widget& sink();

            /** Moves this widget above every sibling and immediately repaints. */
            Widget& raiseToTop();

            /** Moves this widget below every sibling and immediately repaints. */
            Widget& sinkToBottom();

            /** Returns the optional parent that owns this widget. */
            [[nodiscard]] Widget* parentWidget() const noexcept;

            /**
             * Returns direct children in bottom-to-top stacking order, excluding deeper
             * descendants.
             */
            [[nodiscard]] std::vector<Widget*> getChildren() const;

            /**
             * Returns the configured density-independent position relative to the parent
             * widget.
             */
            [[nodiscard]] Position getPlace() const noexcept;

            /**
             * Returns the accumulated density-independent position relative to the
             * window origin.
             */
            [[nodiscard]] Position getAbsolutePlace() const noexcept;

            /** Returns the non-owning native window containing this widget. */
            [[nodiscard]] MainWindow* getWindow() const noexcept;

            /** Returns the configured density-independent component dimensions. */
            [[nodiscard]] Size getDimensions() const noexcept;

            /** Returns the maximum dimensions applied by the active fill axes. */
            [[nodiscard]] Size getMaxSize() const noexcept;

            /** Returns the minimum dimensions applied by the active fill axes. */
            [[nodiscard]] Size getMinSize() const noexcept;

            /** Returns whether this component is configured to render. */
            [[nodiscard]] bool isVisible() const noexcept;

            /** Returns the configured component background color. */
            [[nodiscard]] Color getBackgroundColor() const noexcept;

            /** Returns the configured component corner radius in dp. */
            [[nodiscard]] float getBorderRadius() const noexcept;

            /** Returns the configured component border width in dp. */
            [[nodiscard]] float getBorderWidth() const noexcept;

            /** Returns the configured component border color. */
            [[nodiscard]] Color getBorderColor() const noexcept;

            /** Returns the configured component shadow width in dp. */
            [[nodiscard]] float getShadowWidth() const noexcept;

            /** Returns the configured component shadow color. */
            [[nodiscard]] Color getShadowColor() const noexcept;

            /** Returns the configured Gaussian blur radius in dp. */
            [[nodiscard]] float getBlurRadius() const noexcept;

            /** Returns the configured component opacity. */
            [[nodiscard]] float getAlpha() const noexcept;

            /** Returns whether this component can receive keyboard focus. */
            [[nodiscard]] bool isFocusable() const noexcept;

            /** Returns whether this component currently has effective keyboard focus. */
            [[nodiscard]] bool isFocused() const noexcept;

        protected:
            /**
             * Creates a base widget for a derived component and optional owning parent.
             */
            explicit Widget(Widget* parentWidget = nullptr) noexcept;

            /**
             * Destroys all child widgets before a derived destructor releases its state.
             */
            void destroyChildren() noexcept;

            /**
             * Handles a supported mouse-button press at coordinates local to this
             * widget.
             */
            virtual void handlePointerPress(MouseButton button, Position localPosition);

            /** Handles pointer motion while a supported mouse button remains pressed. */
            virtual void handlePointerDrag(MouseButton button, Position localPosition);
            /**
             * Handles a mouse-wheel scroll at coordinates local to this widget and
             * returns true when the scroll offset was consumed.
             */
            virtual bool handlePointerScroll(Position localPosition, double horizontalOffset, double verticalOffset);

            /**
             * Handles a completed mouse click and its coordinates local to this widget.
             */
            virtual void handleClick(MouseButton button, Position localPosition);

            /**
             * Handles an editing key and GLFW modifier mask routed to the focused
             * widget.
             */
            virtual void handleKeyInput(int key, int modifiers);

            /** Handles one Unicode scalar committed to the focused widget. */
            virtual void handleCharacterInput(std::uint32_t codePoint);

            /** Handles effective keyboard-focus changes while preserving focus events. */
            virtual void handleFocusChanged(bool focused);

            /** Returns native clipboard text, or an empty string when unavailable. */
            [[nodiscard]] std::string clipboardText() const;

            /**
             * Replaces native clipboard text when this widget is attached to a shown
             * window.
             */
            void clipboardText(std::string_view value) const;

            /** Notifies a derived component that its border width changed. */
            virtual void borderWidthChanged();

            /**
             * Marks the cached render payload stale so the next submission rebuilds
             * it; call from any subclass setter that changes rendered content.
             */
            void markRenderDirty() noexcept;

            /**
             * Changes the owning parent widget for this component.
             * Passing nullptr detaches the widget from its current parent without
             * deleting it. The widget is inserted into the new parent's child list and
             * its window association is updated recursively.
             */
            Widget& setParent(Widget* parentWidget);

        private:
            friend class MainWindow;
            friend class Layout;
            friend class RowLayout;
            friend class ColumnLayout;
            friend class ScrollArea;

            /** Removes this widget from its parent's intrusive child list. */
            void detachFromParent() noexcept;

            /**
             * Subscribes to the current parent while at least one fill dimension is
             * active.
             */
            void updateFillSubscription();

            /** Removes the active parent-size subscription, if any. */
            void removeFillSubscription() noexcept;

            /**
             * Applies direct-parent or root-window dimensions to every enabled fill axis.
             */
            void updateFillSizeFromParent();

            /**
             * Applies a parent-assigned size while temporarily bypassing active
             * fill-axis locks.
             */
            void applyParentControlledSize(Size dimensions);

            /**
             * Updates one direct child's filled dimensions without runtime type
             * inspection.
             */
            virtual void updateFilledChild(Widget& child);

            /** Submits a changed sibling stacking order without requiring commit(). */
            void submitStackingOrder();

            /** Notifies this widget, its ancestors, and its owning window of a repaint submission. */
            void notifyRepainted();

            /** Associates this widget subtree with a non-owning window host. */
            void setWindowRecursive(MainWindow* window) noexcept;

            /** Collects renderable descendants with absolute window coordinates. */
            void collectRenderData(std::vector<Internal::WidgetRenderData>& snapshots, Position parentPosition,
                bool parentVisible, float parentOpacity);

            /**
             * Collects renderable descendants while reusing unchanged render payloads
             * from the previous submission; sets structureChanged and leaves snapshots
             * unspecified when the tree no longer matches the previous submission.
             */
            void collectRenderData(std::vector<Internal::WidgetRenderData>& snapshots,
                const Internal::WidgetRenderData*& previous, const Internal::WidgetRenderData* previousEnd,
                bool& structureChanged, Position parentPosition, bool parentVisible, float parentOpacity);

            /**
             * Synchronizes an optional platform-native component with the current window
             * geometry.
             */
            virtual void syncNativeComponent(
                WindowId parentWindow, Position position, Size dimensions, bool visible) const;

            /**
             * Releases an optional platform-native component before its host window is
             * destroyed.
             */
            virtual void destroyNativeComponent() noexcept;

            /**
             * Releases platform-native components throughout this complete child
             * subtree.
             */
            void destroyNativeComponents() noexcept;

            /**
             * Creates component-specific immutable render state, or nullptr for
             * containers.
             */
            [[nodiscard]] virtual std::shared_ptr<const Internal::RenderPayload> renderPayload() const;

            Widget* parentWidget_{};                    ///< Non-owning link to the parent that owns this
                                                        ///< widget.
            Widget* firstChild_{};                      ///< First directly owned child.
            Widget* lastChild_{};                       ///< Last directly owned child.
            Widget* previousSibling_{};                 ///< Previous child owned by the same parent.
            Widget* nextSibling_{};                     ///< Next child owned by the same parent.
            MainWindow* window_{};                      ///< Non-owning native window that renders this subtree.
            Position position_{};                       ///< Position relative to the parent widget in dp.
            Size size_{100, 30};                        ///< Density-independent component dimensions.
            bool visible_{true};                        ///< Whether this widget is locally visible.
            Color background_{0U, 0U, 0U, 0U};          ///< Component background, transparent by default.
            float borderRadius_{};                      ///< Component corner radius in dp.
            float borderWidth_{};                       ///< Component border thickness in dp, zero by default.
            Color borderColor_{148U, 163U, 184U, 255U}; ///< Component border color.
            float shadowWidth_{};                       ///< Component shadow extent in dp, zero by default.
            Color shadowColor_{0U, 0U, 0U, 0U};         ///< Component shadow color, transparent by default.
            float blurRadius_{};                        ///< Gaussian blur radius in dp, zero disables blur.
            float alpha_{1.0F};                         ///< Component opacity multiplier.
            bool focusable_{};                          ///< Whether pointer presses may assign keyboard focus.
            Widget* fillSizeParent_{};                  ///< Parent whose resize event currently drives
                                                        ///< fill dimensions.
            bool fillMaxWidth_{};                       ///< Whether the direct-parent width owns this widget
                                                        ///< width.
            bool fillMaxHeight_{};                      ///< Whether the direct-parent height owns this widget
                                                        ///< height.
            Size minSize_{1, 1};                        ///< Minimum dimensions applied to active fill axes.
            Size maxSize_{std::numeric_limits<int>::max(),
                std::numeric_limits<int>::max()};       ///< Maximum dimensions applied to active fill
                                                        ///< axes.
            bool applyingFillSize_{};                   ///< Whether a parent-driven virtual size call may
                                                        ///< update locked axes.
            std::shared_ptr<Widget*> deletionTarget_{}; ///< Cancelable identity shared with the queued deletion.
            bool destroying_{};                         ///< Guards recursive destruction against reentry.
            bool renderDirty_{true};                    ///< Whether the cached render payload must be
                                                        ///< rebuilt for the next submission.
    };

    /**
     * Provides a transparent, non-interactive component for fixed or flexible
     * layout space.
     */
    class OPENGLGUI_API Spacer final : public Widget
    {
        public:
            /** Creates an empty spacer optionally owned by another widget. */
            explicit Spacer(Widget* parentWidget = nullptr) noexcept;

            /** Releases the spacer and its owned descendants. */
            ~Spacer() override;

            Spacer(const Spacer&) = delete;
            Spacer& operator=(const Spacer&) = delete;
            Spacer(Spacer&&) = delete;
            Spacer& operator=(Spacer&&) = delete;

            /**
             * Sets the position relative to the parent widget in density-independent
             * pixels.
             */
            Spacer& place(Position position) noexcept;

            /** Sets the component dimensions in density-independent pixels. */
            Spacer& size(Size dimensions) override;

            /** Controls whether both dimensions fill the direct parent or root window. */
            Spacer& fillMaxSize(bool enabled);

            /** Controls whether the width fills the direct parent or root window width. */
            Spacer& fillMaxWidth(bool enabled);

            /** Controls whether the height fills the direct parent or root window height. */
            Spacer& fillMaxHeight(bool enabled);

            /** Limits the maximum dimensions applied by the active fill axes. */
            Spacer& maxSize(Size dimensions);

            /** Limits the minimum dimensions applied by the active fill axes. */
            Spacer& minSize(Size dimensions);

            /**
             * Controls whether this component and its descendants participate in
             * rendering.
             */
            Spacer& visible(bool enabled) noexcept;

            /** Sets the component background to a normalized RGBA color. */
            Spacer& backgroundColor(const Color& color);

            /** Sets the component corner radius in density-independent pixels. */
            Spacer& borderRadius(float radius);

            /**
             * Sets the component border width in density-independent pixels; zero
             * disables the border.
             */
            Spacer& borderWidth(float dp);

            /** Sets the component border to a normalized RGBA color. */
            Spacer& borderColor(const Color& color);

            /**
             * Sets the component shadow width in density-independent pixels; zero
             * disables the shadow.
             */
            Spacer& shadowWidth(float dp);

            /** Sets the component shadow to a normalized RGBA color. */
            Spacer& shadowColor(const Color& color);

            /**
             * Sets the Gaussian blur radius in density-independent pixels applied to
             * everything inside the component bounds.
             */
            Spacer& blurRadius(float radius);

            /** Sets the component opacity in the inclusive range [0, 1]. */
            Spacer& alpha(float value);

            /** Controls whether pressing this component can assign keyboard focus. */
            Spacer& focusable(bool enabled) noexcept;

            /**
             * Requests keyboard focus when this component is focusable and attached to a
             * window.
             */
            Spacer& setFocus() noexcept;

            /** Clears keyboard focus when this component is the current window target. */
            Spacer& clearFocus() noexcept;

            /** Submits the complete owning window widget tree for rendering. */
            Spacer& commit();

            /**
             * Moves this widget one level above its next sibling and immediately
             * repaints.
             */
            Spacer& raise();

            /**
             * Moves this widget one level below its previous sibling and immediately
             * repaints.
             */
            Spacer& sink();

            /** Moves this widget above every sibling and immediately repaints. */
            Spacer& raiseToTop();

            /** Moves this widget below every sibling and immediately repaints. */
            Spacer& sinkToBottom();
    };

    /** Displays integer progress from 0 to 100 with a smoothly animated horizontal fill. */
    class OPENGLGUI_API ProgressBar : public Widget
    {
        public:
            /** Creates a 160 by 16 dp progress bar, initially empty and optionally parent-owned. */
            explicit ProgressBar(Widget* parentWidget = nullptr);

            /** Cancels the progress animation and releases owned visual children. */
            ~ProgressBar() override;

            ProgressBar(const ProgressBar&) = delete;
            ProgressBar& operator=(const ProgressBar&) = delete;
            ProgressBar(ProgressBar&&) = delete;
            ProgressBar& operator=(ProgressBar&&) = delete;

            /**
             * Sets the position relative to the parent widget in density-independent
             * pixels.
             */
            ProgressBar& place(Position position) noexcept;

            /** Sets the component dimensions in density-independent pixels. */
            ProgressBar& size(Size dimensions) override;

            /** Controls whether both dimensions fill the direct parent or root window. */
            ProgressBar& fillMaxSize(bool enabled);

            /** Controls whether the width fills the direct parent or root window width. */
            ProgressBar& fillMaxWidth(bool enabled);

            /** Controls whether the height fills the direct parent or root window height. */
            ProgressBar& fillMaxHeight(bool enabled);

            /** Limits the maximum dimensions applied by the active fill axes. */
            ProgressBar& maxSize(Size dimensions);

            /** Limits the minimum dimensions applied by the active fill axes. */
            ProgressBar& minSize(Size dimensions);

            /**
             * Controls whether this component and its descendants participate in
             * rendering.
             */
            ProgressBar& visible(bool enabled) noexcept;

            /** Sets the component background to a normalized RGBA color. */
            ProgressBar& backgroundColor(const Color& color);

            /** Sets the component corner radius in density-independent pixels. */
            ProgressBar& borderRadius(float radius);

            /**
             * Sets the component border width in density-independent pixels; zero
             * disables the border.
             */
            ProgressBar& borderWidth(float dp);

            /** Sets the component border to a normalized RGBA color. */
            ProgressBar& borderColor(const Color& color);

            /**
             * Sets the component shadow width in density-independent pixels; zero
             * disables the shadow.
             */
            ProgressBar& shadowWidth(float dp);

            /** Sets the component shadow to a normalized RGBA color. */
            ProgressBar& shadowColor(const Color& color);

            /**
             * Sets the Gaussian blur radius in density-independent pixels applied to
             * everything inside the component bounds.
             */
            ProgressBar& blurRadius(float radius);

            /** Sets the component opacity in the inclusive range [0, 1]. */
            ProgressBar& alpha(float value);

            /** Controls whether pressing this component can assign keyboard focus. */
            ProgressBar& focusable(bool enabled) noexcept;

            /**
             * Requests keyboard focus when this component is focusable and attached to a
             * window.
             */
            ProgressBar& setFocus() noexcept;

            /** Clears keyboard focus when this component is the current window target. */
            ProgressBar& clearFocus() noexcept;

            /** Submits the complete owning window widget tree for rendering. */
            ProgressBar& commit();

            /**
             * Moves this widget one level above its next sibling and immediately
             * repaints.
             */
            ProgressBar& raise();

            /**
             * Moves this widget one level below its previous sibling and immediately
             * repaints.
             */
            ProgressBar& sink();

            /** Moves this widget above every sibling and immediately repaints. */
            ProgressBar& raiseToTop();

            /** Moves this widget below every sibling and immediately repaints. */
            ProgressBar& sinkToBottom();

            /**
             * Sets the integer target in [0, 100], animating from the current displayed progress over 300 ms.
             * Throws std::invalid_argument outside this range without changing state.
             * Repeating the current target leaves its animation running. Call on the Application thread.
             */
            ProgressBar& progress(int value);

            /** Returns the requested integer progress (default 0), including while its animation runs. */
            [[nodiscard]] int getProgress() const noexcept;

        protected:
            /** Keeps the animated fill inside the current border. */
            void borderWidthChanged() override;

        private:
            /** Owns the refresh-paced interpolation of the displayed percentage. */
            class ProgressAnimation;

            /** Maps the displayed percentage to the current border-inset dimensions. */
            void updateGeometry();

            int progress_{0};                              ///< Requested integer endpoint in [0, 100].
            float displayedProgress_{0.0F};                ///< Animated percentage, independent of component size.
            Widget* fill_{};                               ///< Parent-owned colored portion of the bar.
            std::unique_ptr<ProgressAnimation> animation_; ///< Timeline canceled before visual children are released.
    };

    /** Provides a horizontal, step-quantized numeric slider with value-change events. */
    class OPENGLGUI_API Slider : public Widget
    {
        public:
            /** Creates an horizontal slider optionally owned by another widget. */
            explicit Slider(Widget* parentWidget = nullptr);

            /** Releases the slider and its owned descendants. */
            ~Slider() override;

            Slider(const Slider&) = delete;
            Slider& operator=(const Slider&) = delete;
            Slider(Slider&&) = delete;
            Slider& operator=(Slider&&) = delete;

            /**
             * Sets the position relative to the parent widget in density-independent
             * pixels.
             */
            Slider& place(Position position) noexcept;

            /** Sets the component dimensions in density-independent pixels. */
            Slider& size(Size dimensions) override;

            /** Controls whether both dimensions fill the direct parent or root window. */
            Slider& fillMaxSize(bool enabled);

            /** Controls whether the width fills the direct parent or root window width. */
            Slider& fillMaxWidth(bool enabled);

            /** Controls whether the height fills the direct parent or root window height. */
            Slider& fillMaxHeight(bool enabled);

            /** Limits the maximum dimensions applied by the active fill axes. */
            Slider& maxSize(Size dimensions);

            /** Limits the minimum dimensions applied by the active fill axes. */
            Slider& minSize(Size dimensions);

            /**
             * Controls whether this component and its descendants participate in
             * rendering.
             */
            Slider& visible(bool enabled) noexcept;

            /** Sets the component background to a normalized RGBA color. */
            Slider& backgroundColor(const Color& color);

            /** Sets the component corner radius in density-independent pixels. */
            Slider& borderRadius(float radius);

            /**
             * Sets the component border width in density-independent pixels; zero
             * disables the border.
             */
            Slider& borderWidth(float dp);

            /** Sets the component border to a normalized RGBA color. */
            Slider& borderColor(const Color& color);

            /**
             * Sets the component shadow width in density-independent pixels; zero
             * disables the shadow.
             */
            Slider& shadowWidth(float dp);

            /** Sets the component shadow to a normalized RGBA color. */
            Slider& shadowColor(const Color& color);

            /**
             * Sets the Gaussian blur radius in density-independent pixels applied to
             * everything inside the component bounds.
             */
            Slider& blurRadius(float radius);

            /** Sets the component opacity in the inclusive range [0, 1]. */
            Slider& alpha(float value);

            /** Controls whether pressing this component can assign keyboard focus. */
            Slider& focusable(bool enabled) noexcept;

            /**
             * Requests keyboard focus when this component is focusable and attached to a
             * window.
             */
            Slider& setFocus() noexcept;

            /** Clears keyboard focus when this component is the current window target. */
            Slider& clearFocus() noexcept;

            /** Submits the complete owning window widget tree for rendering. */
            Slider& commit();

            /**
             * Moves this widget one level above its next sibling and immediately
             * repaints.
             */
            Slider& raise();

            /**
             * Moves this widget one level below its previous sibling and immediately
             * repaints.
             */
            Slider& sink();

            /** Moves this widget above every sibling and immediately repaints. */
            Slider& raiseToTop();

            /** Moves this widget below every sibling and immediately repaints. */
            Slider& sinkToBottom();

            /** Value changed; EventData contains the new double value. */
            static constexpr int ON_VALUE_CHANGED = 12;

            /** Sets a finite minimum; raises max if needed and re-snaps the value. Rejects a non-finite span. */
            Slider& min(double minimum);

            /** Sets a finite maximum; lowers min if needed and re-snaps the value. Rejects a non-finite span. */
            Slider& max(double maximum);

            /** Sets a positive finite step anchored at min (default 1), rounding halfway upward. */
            Slider& precision(double step);

            /** Clamps and snaps a finite value; both endpoints remain reachable. Repaints and notifies on change. */
            Slider& value(double current);

            /** Returns the current clamped, snapped value (default 0, within the default range [0, 100]). */
            [[nodiscard]] double getValue() const noexcept;

        protected:
            /** Starts a left-button thumb grab or jumps to the pressed track position. */
            void handlePointerPress(MouseButton button, Position localPosition) override;

            /** Drags the grabbed thumb, clamping motion outside the track. */
            void handlePointerDrag(MouseButton button, Position localPosition) override;

            /** Updates the inset track when the component border changes. */
            void borderWidthChanged() override;

        private:
            /** Repositions the visual children after a value, bounds, or size change. */
            void updateGeometry();

            /** Applies range clamping and step rounding without modifying component state. */
            [[nodiscard]] double normalizedValue(double current) const noexcept;

            /** Synchronizes visuals and emits a double payload only if the normalized value changed. */
            void applyValue(double current);

            /** Converts a pointer coordinate to a value using the retained thumb grab offset. */
            void valueFromPointer(int horizontal);

            double minimum_{0.0};   ///< Inclusive lower bound and step origin.
            double maximum_{100.0}; ///< Inclusive upper bound, also reachable off the step grid.
            double precision_{1.0}; ///< Positive value increment.
            double value_{0.0};     ///< Current normalized value.
            Widget* track_{};       ///< Owned background track, drawn below the fill and thumb.
            Widget* fill_{};        ///< Owned colored portion of the track.
            Widget* thumb_{};       ///< Owned circular draggable handle.
            double trackStart_{};   ///< Minimum thumb-center coordinate in local dp.
            int travel_{};          ///< Available horizontal thumb travel in dp.
            double grabOffset_{};   ///< Pointer offset from the thumb center during dragging.
    };

    /** Provides lifecycle tracking and interpolation helpers for Widget animations. */
    class OPENGLGUI_API WidgetAnimation : public Animation
    {
        public:
            /** Releases the target subscription and any inherited animation state. */
            ~WidgetAnimation() override;

            WidgetAnimation(const WidgetAnimation&) = delete;
            WidgetAnimation& operator=(const WidgetAnimation&) = delete;
            WidgetAnimation(WidgetAnimation&&) = delete;
            WidgetAnimation& operator=(WidgetAnimation&&) = delete;

            /** Returns the target Widget, or nullptr after it has been destroyed. */
            [[nodiscard]] Widget* widget() const noexcept;

        protected:
            /** Creates a Widget-bound timeline configured later through start(). */
            explicit WidgetAnimation(Widget* widget);

            /** Stops tracking the target after it emits ON_DESTROYED. */
            void handleWidgetDestroyed() noexcept;

        private:
            Widget* widget_{}; ///< Non-owning target tracked for the animation lifetime.
    };

    /** Moves one Widget between two positions over a timed Animation. */
    class OPENGLGUI_API MoveAnimation final : public WidgetAnimation
    {
        public:
            /** Creates a movement animation whose endpoints are set by start(). */
            explicit MoveAnimation(Widget* widget);

            /** Releases movement-specific state and the inherited Widget subscription. */
            ~MoveAnimation() override = default;

            MoveAnimation(const MoveAnimation&) = delete;
            MoveAnimation& operator=(const MoveAnimation&) = delete;
            MoveAnimation(MoveAnimation&&) = delete;
            MoveAnimation& operator=(MoveAnimation&&) = delete;

            using WidgetAnimation::start;

            /** Stores the movement endpoints and duration, then starts the timeline. */
            MoveAnimation& start(Position start, Position end, std::chrono::milliseconds duration);

            /**
             * Stores the movement endpoints and an integer millisecond duration, then
             * starts the timeline.
             */
            MoveAnimation& start(Position start, Position end, int durationMilliseconds);

        protected:
            /**
             * Applies the interpolated position and submits the owning tree for repaint.
             */
            void update(float progress) override;

        private:
            Position start_{}; ///< Initial target position, including negative coordinates.
            Position end_{};   ///< Final target position, including negative coordinates.
    };

    /** Resizes one Widget between two dimensions over a timed Animation. */
    class OPENGLGUI_API SizeAnimation final : public WidgetAnimation
    {
        public:
            /** Creates a size animation whose endpoints are set by start(). */
            explicit SizeAnimation(Widget* widget);

            ~SizeAnimation() override = default;

            SizeAnimation(const SizeAnimation&) = delete;
            SizeAnimation& operator=(const SizeAnimation&) = delete;
            SizeAnimation(SizeAnimation&&) = delete;
            SizeAnimation& operator=(SizeAnimation&&) = delete;

            using WidgetAnimation::start;

            /** Stores the size endpoints and duration, then starts the timeline. */
            SizeAnimation& start(Size start, Size end, std::chrono::milliseconds duration);

            /**
             * Stores the size endpoints and an integer millisecond duration, then
             * starts the timeline.
             */
            SizeAnimation& start(Size start, Size end, int durationMilliseconds);

        protected:
            /** Applies the interpolated dimensions and submits the owning tree. */
            void update(float progress) override;

        private:
            Size start_{}; ///< Initial target dimensions.
            Size end_{};   ///< Final target dimensions.
    };

    /** Adjusts one Widget's opacity between two values over a timed Animation. */
    class OPENGLGUI_API AlphaAnimation final : public WidgetAnimation
    {
        public:
            /** Creates an opacity animation whose endpoints are set by start(). */
            explicit AlphaAnimation(Widget* widget);

            ~AlphaAnimation() override = default;

            AlphaAnimation(const AlphaAnimation&) = delete;
            AlphaAnimation& operator=(const AlphaAnimation&) = delete;
            AlphaAnimation(AlphaAnimation&&) = delete;
            AlphaAnimation& operator=(AlphaAnimation&&) = delete;

            using WidgetAnimation::start;

            /** Stores the opacity endpoints and duration, then starts the timeline. */
            AlphaAnimation& start(float start, float end, std::chrono::milliseconds duration);

            /**
             * Stores the opacity endpoints and an integer millisecond duration, then
             * starts the timeline.
             */
            AlphaAnimation& start(float start, float end, int durationMilliseconds);

        protected:
            /** Applies the interpolated opacity and submits the owning tree. */
            void update(float progress) override;

        private:
            float start_{}; ///< Initial target opacity.
            float end_{};   ///< Final target opacity.
    };

    /** Adjusts one Widget's shadow width, and optionally its shadow color. */
    class OPENGLGUI_API ShadowAnimation final : public WidgetAnimation
    {
        public:
            /** Creates a shadow animation whose endpoints are set by start(). */
            explicit ShadowAnimation(Widget* widget);

            ~ShadowAnimation() override = default;

            ShadowAnimation(const ShadowAnimation&) = delete;
            ShadowAnimation& operator=(const ShadowAnimation&) = delete;
            ShadowAnimation(ShadowAnimation&&) = delete;
            ShadowAnimation& operator=(ShadowAnimation&&) = delete;

            using WidgetAnimation::start;

            /**
             * Stores the shadow-width endpoints and duration, then starts the timeline
             * while keeping the current shadow color.
             */
            ShadowAnimation& start(float startWidth, float endWidth, std::chrono::milliseconds duration);

            /**
             * Stores the shadow-width endpoints and an integer millisecond duration,
             * then starts the timeline while keeping the current shadow color.
             */
            ShadowAnimation& start(float startWidth, float endWidth, int durationMilliseconds);

            /**
             * Stores the shadow width and color endpoints and duration, then starts
             * the timeline.
             */
            ShadowAnimation& start(
                float startWidth, float endWidth, Color startColor, Color endColor, std::chrono::milliseconds duration);

            /**
             * Stores the shadow width and color endpoints and an integer millisecond
             * duration, then starts the timeline.
             */
            ShadowAnimation& start(
                float startWidth, float endWidth, Color startColor, Color endColor, int durationMilliseconds);

        protected:
            /** Applies the interpolated shadow state and submits the owning tree. */
            void update(float progress) override;

        private:
            float startWidth_{};  ///< Initial target shadow width.
            float endWidth_{};    ///< Final target shadow width.
            Color startColor_{};  ///< Initial shadow color when color animation is enabled.
            Color endColor_{};    ///< Final shadow color when color animation is enabled.
            bool animateColor_{}; ///< Whether shadow color should be interpolated.
    };

    /**
     * Adjusts one Widget's Gaussian blur radius between two values over a timed
     * Animation.
     */
    class OPENGLGUI_API BlurAnimation final : public WidgetAnimation
    {
        public:
            /** Creates a blur-radius animation whose endpoints are set by start(). */
            explicit BlurAnimation(Widget* widget);

            ~BlurAnimation() override = default;

            BlurAnimation(const BlurAnimation&) = delete;
            BlurAnimation& operator=(const BlurAnimation&) = delete;
            BlurAnimation(BlurAnimation&&) = delete;
            BlurAnimation& operator=(BlurAnimation&&) = delete;

            using WidgetAnimation::start;

            /**
             * Stores the blur-radius endpoints and duration, then starts the timeline.
             */
            BlurAnimation& start(float start, float end, std::chrono::milliseconds duration);

            /**
             * Stores the blur-radius endpoints and an integer millisecond duration,
             * then starts the timeline.
             */
            BlurAnimation& start(float start, float end, int durationMilliseconds);

        protected:
            /** Applies the interpolated blur radius and submits the owning tree. */
            void update(float progress) override;

        private:
            float start_{}; ///< Initial target blur radius in dp.
            float end_{};   ///< Final target blur radius in dp.
    };

/**
 * Hosts an empty platform-native child component for external native
 * integrations.
 */
#if defined(_MSC_VER) && !defined(OPENGLGUI_STATIC)
#pragma warning(push)
#pragma warning(disable : 4251)
#endif
    class OPENGLGUI_API NativeWidget : public Widget
    {
        public:
            /**
             * Creates a focusable native-component host optionally owned by another
             * widget.
             */
            explicit NativeWidget(Widget* parentWidget = nullptr);

            /** Releases the platform-native child component. */
            ~NativeWidget() override;

            NativeWidget(const NativeWidget&) = delete;
            NativeWidget& operator=(const NativeWidget&) = delete;
            NativeWidget(NativeWidget&&) = delete;
            NativeWidget& operator=(NativeWidget&&) = delete;

            /** Sets both retained and native component dimensions immediately. */
            NativeWidget& size(Size dimensions) override;

            /**
             * Sets the position relative to the parent widget in density-independent
             * pixels.
             */
            NativeWidget& place(Position position) noexcept;

            /** Controls whether both dimensions fill the direct parent or root window. */
            NativeWidget& fillMaxSize(bool enabled);

            /** Controls whether the width fills the direct parent or root window width. */
            NativeWidget& fillMaxWidth(bool enabled);

            /** Controls whether the height fills the direct parent or root window height. */
            NativeWidget& fillMaxHeight(bool enabled);

            /** Limits the maximum dimensions applied by the active fill axes. */
            NativeWidget& maxSize(Size dimensions);

            /** Limits the minimum dimensions applied by the active fill axes. */
            NativeWidget& minSize(Size dimensions);

            /**
             * Controls whether this component and its descendants participate in
             * rendering.
             */
            NativeWidget& visible(bool enabled) noexcept;

            /** Sets the component background to a normalized RGBA color. */
            NativeWidget& backgroundColor(const Color& color);

            /** Sets the component corner radius in density-independent pixels. */
            NativeWidget& borderRadius(float radius);

            /**
             * Sets the component border width in density-independent pixels; zero
             * disables the border.
             */
            NativeWidget& borderWidth(float dp);

            /** Sets the component border to a normalized RGBA color. */
            NativeWidget& borderColor(const Color& color);

            /**
             * Sets the component shadow width in density-independent pixels; zero
             * disables the shadow.
             */
            NativeWidget& shadowWidth(float dp);

            /** Sets the component shadow to a normalized RGBA color. */
            NativeWidget& shadowColor(const Color& color);

            /**
             * Sets the Gaussian blur radius in density-independent pixels applied to
             * everything inside the component bounds.
             */
            NativeWidget& blurRadius(float radius);

            /** Sets the component opacity in the inclusive range [0, 1]. */
            NativeWidget& alpha(float value);

            /** Controls whether pressing this component can assign keyboard focus. */
            NativeWidget& focusable(bool enabled) noexcept;

            /**
             * Requests keyboard focus when this component is focusable and attached to a
             * window.
             */
            NativeWidget& setFocus() noexcept;

            /** Clears keyboard focus when this component is the current window target. */
            NativeWidget& clearFocus() noexcept;

            /** Submits the complete owning window widget tree for rendering. */
            NativeWidget& commit();

            /**
             * Moves this widget one level above its next sibling and immediately
             * repaints.
             */
            NativeWidget& raise();

            /**
             * Moves this widget one level below its previous sibling and immediately
             * repaints.
             */
            NativeWidget& sink();

            /** Moves this widget above every sibling and immediately repaints. */
            NativeWidget& raiseToTop();

            /** Moves this widget below every sibling and immediately repaints. */
            NativeWidget& sinkToBottom();

            /**
             * Returns the platform-native child handle, or zero before creation or on
             * unsupported backends.
             */
            [[nodiscard]] NativeWidgetId windowId() const noexcept;

        private:
            /**
             * Creates or updates the platform-native child without changing native
             * keyboard focus.
             */
            void syncNativeComponent(
                WindowId parentWindow, Position position, Size dimensions, bool visible) const override;

            /** Destroys the platform-native child without affecting MainWindow focus. */
            void destroyNativeComponent() noexcept override;

            class Impl;
            std::unique_ptr<Impl> impl_; ///< Platform-specific native child state.
    };

#if defined(_MSC_VER) && !defined(OPENGLGUI_STATIC)
#pragma warning(pop)
#endif

/** Provides common child management and alignment configuration for layouts. */
#if defined(_MSC_VER) && !defined(OPENGLGUI_STATIC)
#pragma warning(push)
#pragma warning(disable : 4251)
#endif
    class OPENGLGUI_API Layout : public Widget
    {
        public:
            /** Creates a layout optionally owned by another widget. */
            explicit Layout(Widget* parentWidget = nullptr) noexcept;

            /** Unsubscribes from child resize and destruction events. */
            ~Layout() override;

            Layout(const Layout&) = delete;
            Layout& operator=(const Layout&) = delete;
            Layout(Layout&&) = delete;
            Layout& operator=(Layout&&) = delete;

            /**
             * Sets the position relative to the parent widget in density-independent
             * pixels.
             */
            Layout& place(Position position) noexcept;

            /** Controls whether both dimensions fill the direct parent or root window. */
            Layout& fillMaxSize(bool enabled);

            /** Controls whether the width fills the direct parent or root window width. */
            Layout& fillMaxWidth(bool enabled);

            /** Controls whether the height fills the direct parent or root window height. */
            Layout& fillMaxHeight(bool enabled);

            /** Limits the maximum dimensions applied by the active fill axes. */
            Layout& maxSize(Size dimensions);

            /** Limits the minimum dimensions applied by the active fill axes. */
            Layout& minSize(Size dimensions);

            /**
             * Controls whether this component and its descendants participate in
             * rendering.
             */
            Layout& visible(bool enabled) noexcept;

            /** Sets the component background to a normalized RGBA color. */
            Layout& backgroundColor(const Color& color);

            /** Sets the component corner radius in density-independent pixels. */
            Layout& borderRadius(float radius);

            /**
             * Sets the component border width in density-independent pixels; zero
             * disables the border.
             */
            Layout& borderWidth(float dp);

            /** Sets the component border to a normalized RGBA color. */
            Layout& borderColor(const Color& color);

            /**
             * Sets the component shadow width in density-independent pixels; zero
             * disables the shadow.
             */
            Layout& shadowWidth(float dp);

            /** Sets the component shadow to a normalized RGBA color. */
            Layout& shadowColor(const Color& color);

            /**
             * Sets the Gaussian blur radius in density-independent pixels applied to
             * everything inside the component bounds.
             */
            Layout& blurRadius(float radius);

            /** Sets the component opacity in the inclusive range [0, 1]. */
            Layout& alpha(float value);

            /** Controls whether pressing this component can assign keyboard focus. */
            Layout& focusable(bool enabled) noexcept;

            /**
             * Requests keyboard focus when this component is focusable and attached to a
             * window.
             */
            Layout& setFocus() noexcept;

            /** Clears keyboard focus when this component is the current window target. */
            Layout& clearFocus() noexcept;

            /** Submits the complete owning window widget tree for rendering. */
            Layout& commit();

            /**
             * Moves this widget one level above its next sibling and immediately
             * repaints.
             */
            Layout& raise();

            /**
             * Moves this widget one level below its previous sibling and immediately
             * repaints.
             */
            Layout& sink();

            /** Moves this widget above every sibling and immediately repaints. */
            Layout& raiseToTop();

            /** Moves this widget below every sibling and immediately repaints. */
            Layout& sinkToBottom();

            /** Sets layout padding in density-independent pixels. */
            Layout& padding(Padding value);

            /** Sets the gap between adjacent children in density-independent pixels. */
            Layout& spacing(int value);

            /** Sets horizontal and vertical alignment flags. */
            Layout& alignment(unsigned int flags);

            /** Adds a child widget and transfers ownership to this layout. */
            Layout& addWidget(Widget* widget);

            /** Removes and recursively destroys a widget owned by this layout. */
            Layout& removeWidget(Widget* widget);

            /**
             * Removes every layout item and recursively destroys all retained widgets.
             */
            Layout& clear() noexcept;

            /** Returns the configured layout padding. */
            [[nodiscard]] Padding getPadding() const noexcept;

            /** Returns the configured child gap in dp. */
            [[nodiscard]] int getSpacing() const noexcept;

            /** Returns the configured alignment flags. */
            [[nodiscard]] unsigned int getAlignment() const noexcept;

            /** Resizes the layout and immediately recomputes child positions. */
            Layout& size(Size dimensions) override;

        protected:
            /** Arranges direct children according to the concrete layout direction. */
            virtual void arrangeChildren() = 0;

            /**
             * Runs the shared main/cross-axis allocation algorithm for a horizontal or
             * vertical layout.
             */
            void arrangeChildrenAlongAxis(bool horizontal);

            /** Returns the layout's current content rectangle after padding. */
            [[nodiscard]] Size contentSize() const noexcept;

            Padding padding_{};            ///< Configured edge padding in dp.
            int spacing_{};                ///< Configured gap between adjacent layout items in dp.
            unsigned int alignment_{};     ///< Configured horizontal and vertical alignment flags.
            std::vector<Widget*> items_{}; ///< Direct child widgets in insertion order,
                                           ///< including Spacer instances.

        private:
            friend class Widget;

            /**
             * Reflows Layout-managed children when one requests parent-controlled fill
             * sizing.
             */
            void updateFilledChild(Widget& child) override;

            /** Removes subscriptions for one child that is being destroyed. */
            void unsubscribeChild(Widget* child) noexcept;

            /** Subscribes to one direct child's resize and destruction notifications. */
            void subscribeChild(Widget* child);

            /**
             * Removes the retained item that references a child without destroying it.
             */
            bool eraseWidgetItem(Widget* child) noexcept;

            std::vector<Widget*> subscribedChildren_{}; ///< Direct children currently subscribed to by
                                                        ///< this layout.
    };

#if defined(_MSC_VER) && !defined(OPENGLGUI_STATIC)
#pragma warning(pop)
#endif

    /** Arranges direct children from top to bottom. */
    class OPENGLGUI_API ColumnLayout final : public Layout
    {
        public:
            /** Creates a vertical layout optionally owned by another widget. */
            explicit ColumnLayout(Widget* parentWidget = nullptr) noexcept;

        protected:
            /** Arranges direct children vertically within the layout rectangle. */
            void arrangeChildren() override;
    };

    /** Arranges direct children from left to right. */
    class OPENGLGUI_API RowLayout final : public Layout
    {
        public:
            /** Creates a horizontal layout optionally owned by another widget. */
            explicit RowLayout(Widget* parentWidget = nullptr) noexcept;

        protected:
            /** Arranges direct children horizontally within the layout rectangle. */
            void arrangeChildren() override;
    };

    /** Selects how a label image occupies its component rectangle. */
    enum class ImageMode {
        Center,  ///< Draws the image at its original size and centers it.
        Stretch, ///< Stretches the image independently to the label width and
                 ///< height.
        Fill,    ///< Preserves aspect ratio and crops the image to fill the label.
    };

    /** Selects the physical display subpixel order used for text antialiasing. */
    enum class SubpixelRendering {
        Disabled, ///< Uses grayscale antialiasing suitable for unknown display
                  ///< layouts.
        Rgb,      ///< Uses left-to-right red, green, and blue subpixel coverage.
        Bgr,      ///< Uses left-to-right blue, green, and red subpixel coverage.
    };

/** Owns decoded RGBA image pixels loaded from PNG or JPEG data. */
#if defined(_MSC_VER) && !defined(OPENGLGUI_STATIC)
#pragma warning(push)
// Standard-library storage remains private behind the DLL interface.
#pragma warning(disable : 4251)
#endif
    class OPENGLGUI_API Image
    {
        public:
            /** Creates an empty image. */
            Image() noexcept;

            /** Decodes an encoded PNG or JPEG byte sequence. */
            Image(const unsigned char* encodedBytes, std::size_t byteCount);

            /** Decodes a complete xxd-style encoded image array. */
            template <std::size_t ByteCount>
            explicit Image(const unsigned char (&encodedBytes)[ByteCount]) : Image(encodedBytes, ByteCount)
            { }

            /** Shares immutable decoded pixels with a copied image. */
            Image(const Image& other) noexcept;

            /** Transfers decoded pixels from another image. */
            Image(Image&& other) noexcept;

            /** Releases shared decoded pixel storage. */
            ~Image();

            Image& operator=(const Image& other) noexcept;
            Image& operator=(Image&& other) noexcept;

            /** Reads an encoded PNG or JPEG with fopen and replaces this image. */
            Image& load(std::string_view filePath);

            /** Decodes encoded PNG or JPEG bytes and replaces this image. */
            Image& load(const unsigned char* encodedBytes, std::size_t byteCount);

            /** Decodes a complete xxd-style encoded image array. */
            template <std::size_t ByteCount>
            Image& load(const unsigned char (&encodedBytes)[ByteCount])
            {
                return load(encodedBytes, ByteCount);
            }

            /** Releases the decoded image and returns to the empty state. */
            void clear() noexcept;

            /** Returns true when no decoded image is available. */
            [[nodiscard]] bool empty() const noexcept;

            /** Returns the decoded width in pixels, or zero for an empty image. */
            [[nodiscard]] int width() const noexcept;

            /** Returns the decoded height in pixels, or zero for an empty image. */
            [[nodiscard]] int height() const noexcept;

            /** Returns the immutable decoded RGBA pixels, or nullptr when empty. */
            [[nodiscard]] const unsigned char* pixels() const noexcept;

            /** Returns the decoded RGBA byte count. */
            [[nodiscard]] std::size_t pixelByteSize() const noexcept;

        private:
            class Impl;

            /** Decodes PNG or JPEG bytes into immutable RGBA storage. */
            static std::shared_ptr<const Impl> decode(const unsigned char* encodedBytes, std::size_t byteCount);

            /** Copies decoder-owned RGBA pixels into immutable image storage. */
            static std::shared_ptr<const Impl> makeDecoded(unsigned char* decoded, int width, int height);

            std::shared_ptr<const Impl> impl_; ///< Shared immutable decoded image storage.
    };

#if defined(_MSC_VER) && !defined(OPENGLGUI_STATIC)
#pragma warning(pop)
#endif

/** Owns one native GLFW window and its OpenGL renderer. */
#if defined(_MSC_VER) && !defined(OPENGLGUI_STATIC)
#pragma warning(push)
// The PImpl pointer is never accessed by value across the DLL boundary.
#pragma warning(disable : 4251)
#endif
    class OPENGLGUI_API MainWindow
    {
        public:
            /** Identifies the basic events emitted by a MainWindow sender. */
            enum class EventId : int {
                ON_ENTERED = 1,        ///< Pointer entered this native window.
                ON_EXITED = 2,         ///< Pointer left this native window.
                ON_CLOSED = 3,         ///< Native window close notification.
                ON_DESTROYED = 4,      ///< Window object is being destroyed.
                ON_RESIZED = 5,        ///< Native client dimensions changed.
                ON_KEY_PRESSED = 7,    ///< A key was pressed while this window was active.
                ON_REPAINTED = 8,      ///< Window repaint was submitted.
                ON_GOT_FOCUS = 9,      ///< Native window acquired keyboard focus.
                ON_LOST_FOCUS = 10,    ///< Native window released keyboard focus.
                ON_FOCUS_CHANGED = 11, ///< Effective widget focus changed; payload is Widget* or nullptr.
                ON_STATE_CHANGED = 12, ///< Native minimized/maximized state changed; no payload.
            };

            static constexpr int ON_ENTERED =
                static_cast<int>(EventId::ON_ENTERED); ///< Integer alias for EventId::ON_ENTERED.
            static constexpr int ON_EXITED =
                static_cast<int>(EventId::ON_EXITED); ///< Integer alias for EventId::ON_EXITED.
            static constexpr int ON_CLOSED =
                static_cast<int>(EventId::ON_CLOSED); ///< Integer alias for EventId::ON_CLOSED.
            static constexpr int ON_DESTROYED =
                static_cast<int>(EventId::ON_DESTROYED); ///< Integer alias for EventId::ON_DESTROYED.
            static constexpr int ON_RESIZED =
                static_cast<int>(EventId::ON_RESIZED); ///< Integer alias for EventId::ON_RESIZED.
            static constexpr int ON_KEY_PRESSED =
                static_cast<int>(EventId::ON_KEY_PRESSED); ///< Integer alias for EventId::ON_KEY_PRESSED.
            static constexpr int ON_REPAINTED =
                static_cast<int>(EventId::ON_REPAINTED); ///< Integer alias for EventId::ON_REPAINTED.
            static constexpr int ON_GOT_FOCUS =
                static_cast<int>(EventId::ON_GOT_FOCUS); ///< Integer alias for EventId::ON_GOT_FOCUS.
            static constexpr int ON_LOST_FOCUS =
                static_cast<int>(EventId::ON_LOST_FOCUS); ///< Integer alias for EventId::ON_LOST_FOCUS.
            static constexpr int ON_FOCUS_CHANGED =
                static_cast<int>(EventId::ON_FOCUS_CHANGED); ///< Integer alias for EventId::ON_FOCUS_CHANGED.
            static constexpr int ON_STATE_CHANGED =
                static_cast<int>(EventId::ON_STATE_CHANGED); ///< Integer alias for EventId::ON_STATE_CHANGED.

            /** Creates a hidden independent 640x480 native window. */
            MainWindow();

            /** Releases native and rendering resources owned by this window. */
            ~MainWindow();

            MainWindow(const MainWindow&) = delete;
            MainWindow& operator=(const MainWindow&) = delete;
            MainWindow(MainWindow&&) = delete;
            MainWindow& operator=(MainWindow&&) = delete;

            /** Installs and takes ownership of one parentless root widget. */
            MainWindow& setWidget(Widget* widget);

            /**
             * Returns the currently owned root widget, or nullptr when none is
             * installed.
             */
            [[nodiscard]] Widget* getWidget() const noexcept;

            /**
             * Returns the non-owning effective keyboard target on the application thread.
             * Returns nullptr when no widget is selected or the native window is inactive;
             * the selected target is retained for restoration when native focus returns.
             */
            [[nodiscard]] Widget* focusedWidget() const noexcept;

            /**
             * Sets the Widget region that drags an undecorated window, or nullptr to
             * disable dragging.
             */
            MainWindow& setTitleWidget(Widget* widget);

            /** Returns the non-owning drag-region Widget configured for this window. */
            [[nodiscard]] Widget* getTitleWidget() const noexcept;

            /**
             * Sets the initial native virtual-screen position and returns this window
             * for chaining.
             */
            MainWindow& place(Position position) noexcept;

            /**
             * Sets the client size in density-independent pixels and returns this window
             * for chaining.
             */
            MainWindow& size(Size dimensions);

            /** Sets the native window title and returns this window for chaining. */
            MainWindow& title(std::string_view text);

            /** Sets the OpenGL-rendered background to a normalized RGBA color. */
            MainWindow& backgroundColor(const Color& color);

            /** Sets the rendered corner radius in density-independent pixels. */
            MainWindow& borderRadius(float radius);

            /** Selects whether the native operating-system frame is visible. */
            MainWindow& decorated(bool enabled) noexcept;

            /** Selects whether the user can resize the native window. */
            MainWindow& resizable(bool enabled) noexcept;

            /** Controls whether showing this window requests native keyboard focus. */
            MainWindow& focusOnShow(bool enabled) noexcept;

            /** Controls whether the native window remains above non-floating windows. */
            MainWindow& alwaysOnTop(bool enabled) noexcept;

            /**
             * Applies native properties and asynchronously submits the complete widget tree.
             * Return and ON_REPAINTED do not imply that the frame has been presented.
             */
            MainWindow& commit();

            /** Creates the native window, starts rendering, and returns immediately. */
            void show();

            /** Requests native minimization on the application thread; no-op before show(). */
            void minimize();

            /** Requests native maximization on the application thread; no-op before show() or if not resizable. */
            void maximize();

            /** Restores an iconified or maximized window on the application thread; no-op before show(). */
            void restore();

            /** Returns the last native minimized state, false before show(); read on the application thread. */
            [[nodiscard]] bool isMinimized() const noexcept;

            /** Returns the last native maximized state, false before show(); read on the application thread. */
            [[nodiscard]] bool isMaximized() const noexcept;

            /**
             * Requests close and automatic recursive destruction during event
             * processing.
             */
            void close() noexcept;

            /** Returns the configured native virtual-screen position. */
            [[nodiscard]] Position getPlace() const noexcept;

            /** Returns the screen position of the client-area origin. */
            [[nodiscard]] Position getClientPlace() const noexcept;

            /** Maps a client-area point in dp to native virtual-screen coordinates. */
            [[nodiscard]] Position mapToScreen(Position clientPosition) const noexcept;

            /** Returns whether the native window currently owns keyboard focus. */
            [[nodiscard]] bool isFocused() const noexcept;

            /** Returns the configured density-independent client size. */
            [[nodiscard]] Size getDimensions() const noexcept;

            /** Returns the configured normalized background color. */
            [[nodiscard]] Color getBackgroundColor() const noexcept;

            /** Returns the configured corner radius in density-independent pixels. */
            [[nodiscard]] float getBorderRadius() const noexcept;

            /**
             * Returns true while a native window exists and has not been asked to close.
             */
            [[nodiscard]] bool isOpen() const noexcept;

            /**
             * Returns the GLFW window identifier, or nullptr before show(); call on the
             * application thread.
             */
            [[nodiscard]] WindowId windowId() const noexcept;

        private:
            friend class Widget;
            friend void Internal::endAnimationBatch();

            /** Submits the current root widget tree to the render thread. */
            void submitWidgets();

            /**
             * Marks the widget tree structure as changed so the next submission
             * performs a full collection instead of an incremental payload update.
             */
            void markWidgetStructureDirty() noexcept;

            /** Clears transient input state before a widget begins destruction. */
            void widgetWillDestroy(Widget* widget) noexcept;

            /** Assigns focus to an eligible Widget attached to this window. */
            void focusWidget(Widget* widget) noexcept;

            /** Clears focus only when the supplied Widget is the retained target. */
            void clearWidgetFocus(Widget* widget) noexcept;

            /**
             * Returns whether the supplied Widget owns effective focus in this window.
             */
            [[nodiscard]] bool isWidgetFocused(const Widget* widget) const noexcept;

            /** Handles a widget detaching or being destroyed inside the owned tree. */
            void widgetDestroyed(Widget* widget) noexcept;

            /**
             * Returns UTF-8 text from the native clipboard, or an empty string when
             * unavailable.
             */
            [[nodiscard]] std::string clipboardText() const;

            /** Replaces native clipboard text for the shown window. */
            void clipboardText(std::string_view value) const;

            class Impl;
            std::unique_ptr<Impl> impl_; ///< Private implementation that hides GLFW from
                                         ///< the public header.
    };

#if defined(_MSC_VER) && !defined(OPENGLGUI_STATIC)
#pragma warning(pop)
#endif

/** Renders aligned text and an optional image inside a parent window. */
#if defined(_MSC_VER) && !defined(OPENGLGUI_STATIC)
#pragma warning(push)
// The PImpl pointer is never accessed by value across the DLL boundary.
#pragma warning(disable : 4251)
#endif
    class OPENGLGUI_API Label : public Widget
    {
        public:
            /** Creates a label optionally owned by another widget. */
            explicit Label(Widget* parentWidget = nullptr);

            /** Destroys child widgets and releases retained label content. */
            ~Label();

            Label(const Label&) = delete;
            Label& operator=(const Label&) = delete;
            Label(Label&&) = delete;
            Label& operator=(Label&&) = delete;

            /**
             * Sets the position relative to the parent widget in density-independent
             * pixels.
             */
            Label& place(Position position) noexcept;

            /** Sets the component dimensions in density-independent pixels. */
            Label& size(Size dimensions) override;

            /** Controls whether both dimensions fill the direct parent or root window. */
            Label& fillMaxSize(bool enabled);

            /** Controls whether the width fills the direct parent or root window width. */
            Label& fillMaxWidth(bool enabled);

            /** Controls whether the height fills the direct parent or root window height. */
            Label& fillMaxHeight(bool enabled);

            /** Limits the maximum dimensions applied by the active fill axes. */
            Label& maxSize(Size dimensions);

            /** Limits the minimum dimensions applied by the active fill axes. */
            Label& minSize(Size dimensions);

            /**
             * Controls whether this component and its descendants participate in
             * rendering.
             */
            Label& visible(bool enabled) noexcept;

            /** Sets the component background to a normalized RGBA color. */
            Label& backgroundColor(const Color& color);

            /** Sets the component corner radius in density-independent pixels. */
            Label& borderRadius(float radius);

            /**
             * Sets the component border width in density-independent pixels; zero
             * disables the border.
             */
            Label& borderWidth(float dp);

            /** Sets the component border to a normalized RGBA color. */
            Label& borderColor(const Color& color);

            /**
             * Sets the component shadow width in density-independent pixels; zero
             * disables the shadow.
             */
            Label& shadowWidth(float dp);

            /** Sets the component shadow to a normalized RGBA color. */
            Label& shadowColor(const Color& color);

            /**
             * Sets the Gaussian blur radius in density-independent pixels applied to
             * everything inside the component bounds.
             */
            Label& blurRadius(float radius);

            /** Sets the component opacity in the inclusive range [0, 1]. */
            Label& alpha(float value);

            /** Controls whether pressing this component can assign keyboard focus. */
            Label& focusable(bool enabled) noexcept;

            /**
             * Requests keyboard focus when this component is focusable and attached to a
             * window.
             */
            Label& setFocus() noexcept;

            /** Clears keyboard focus when this component is the current window target. */
            Label& clearFocus() noexcept;

            /** Submits the complete owning window widget tree for rendering. */
            Label& commit();

            /**
             * Moves this widget one level above its next sibling and immediately
             * repaints.
             */
            Label& raise();

            /**
             * Moves this widget one level below its previous sibling and immediately
             * repaints.
             */
            Label& sink();

            /** Moves this widget above every sibling and immediately repaints. */
            Label& raiseToTop();

            /** Moves this widget below every sibling and immediately repaints. */
            Label& sinkToBottom();

            /** Sets UTF-8 text rendered over the optional image. */
            Label& text(std::string_view value);

            /** Sets an image whose decoded pixels are shared with this label. */
            Label& image(const Image& value) noexcept;

            /** Removes the image while retaining the label text. */
            Label& clearImage() noexcept;

            /** Sets text alignment using the shared Layout alignment flags. */
            Label& textAlignment(unsigned int alignment) noexcept;

            /** Sets image centering, stretching, or fill behavior. */
            Label& imageMode(ImageMode mode) noexcept;

            /** Sets the text to a normalized RGBA color. */
            Label& textColor(const Color& color);

            /** Sets text height in density-independent pixels. */
            Label& fontSize(float dp);

            /** Sets the numeric font weight in the inclusive range [100, 900]. */
            Label& fontWeight(unsigned int weight);

            /** Selects a registered application font family. */
            Label& fontFamily(std::string_view familyName);

            /** Returns the selected application font family. */
            [[nodiscard]] std::string getFontFamily() const;

            /** Selects grayscale, RGB, or BGR subpixel text antialiasing. */
            Label& subpixelRendering(SubpixelRendering mode) noexcept;

            /** Selects single-line clipping or automatic multiline text wrapping. */
            Label& multiLine(bool enabled = true) noexcept;

            /**
             * Sets the maximum number of Unicode characters rendered by this label.
             * std::string::npos disables the limit.
             */
            Label& maxCharacter(std::size_t count) noexcept;

            /** Returns a copy of the configured UTF-8 text. */
            [[nodiscard]] std::string getText() const;

            /** Returns a shared copy of the configured image. */
            [[nodiscard]] Image getImage() const noexcept;

            /** Returns the configured text alignment flags. */
            [[nodiscard]] unsigned int getTextAlignment() const noexcept;

            /** Returns the configured image placement mode. */
            [[nodiscard]] ImageMode getImageMode() const noexcept;

            /** Returns the configured text color. */
            [[nodiscard]] Color getTextColor() const noexcept;

            /** Returns the configured text height in density-independent pixels. */
            [[nodiscard]] float getFontSize() const noexcept;

            /** Returns the configured numeric font weight. */
            [[nodiscard]] unsigned int getFontWeight() const noexcept;

            /** Returns the configured text antialiasing mode. */
            [[nodiscard]] SubpixelRendering getSubpixelRendering() const noexcept;

            /** Returns whether automatic multiline text wrapping is enabled. */
            [[nodiscard]] bool isMultiLine() const noexcept;

            /**
             * Returns the maximum number of Unicode characters rendered by this label,
             * or std::string::npos when no limit is configured.
             */
            [[nodiscard]] std::size_t getMaxCharacter() const noexcept;

        private:
            /** Creates immutable Label-specific render properties. */
            [[nodiscard]] std::shared_ptr<const Internal::RenderPayload> renderPayload() const override;

            class Impl;
            std::unique_ptr<Impl> impl_; ///< Private Label-specific component state.
    };

#if defined(_MSC_VER) && !defined(OPENGLGUI_STATIC)
#pragma warning(pop)
#endif

/** Renders centered text and a clipped ripple when the user clicks it. */
#if defined(_MSC_VER) && !defined(OPENGLGUI_STATIC)
#pragma warning(push)
// The PImpl pointer is never accessed by value across the DLL boundary.
#pragma warning(disable : 4251)
#endif
    class OPENGLGUI_API Button : public Widget
    {
        public:
            /** Creates a button optionally owned by another widget. */
            explicit Button(Widget* parentWidget = nullptr);

            /** Destroys child widgets and releases retained button content. */
            ~Button() override;

            Button(const Button&) = delete;
            Button& operator=(const Button&) = delete;
            Button(Button&&) = delete;
            Button& operator=(Button&&) = delete;

            /**
             * Sets the position relative to the parent widget in density-independent
             * pixels.
             */
            Button& place(Position position) noexcept;

            /** Sets the component dimensions in density-independent pixels. */
            Button& size(Size dimensions) override;

            /** Controls whether both dimensions fill the direct parent or root window. */
            Button& fillMaxSize(bool enabled);

            /** Controls whether the width fills the direct parent or root window width. */
            Button& fillMaxWidth(bool enabled);

            /** Controls whether the height fills the direct parent or root window height. */
            Button& fillMaxHeight(bool enabled);

            /** Limits the maximum dimensions applied by the active fill axes. */
            Button& maxSize(Size dimensions);

            /** Limits the minimum dimensions applied by the active fill axes. */
            Button& minSize(Size dimensions);

            /**
             * Controls whether this component and its descendants participate in
             * rendering.
             */
            Button& visible(bool enabled) noexcept;

            /** Sets the component background to a normalized RGBA color. */
            Button& backgroundColor(const Color& color);

            /** Sets the component corner radius in density-independent pixels. */
            Button& borderRadius(float radius);

            /**
             * Sets the component border width in density-independent pixels; zero
             * disables the border.
             */
            Button& borderWidth(float dp);

            /** Sets the component border to a normalized RGBA color. */
            Button& borderColor(const Color& color);

            /**
             * Sets the component shadow width in density-independent pixels; zero
             * disables the shadow.
             */
            Button& shadowWidth(float dp);

            /** Sets the component shadow to a normalized RGBA color. */
            Button& shadowColor(const Color& color);

            /**
             * Sets the Gaussian blur radius in density-independent pixels applied to
             * everything inside the component bounds.
             */
            Button& blurRadius(float radius);

            /** Sets the component opacity in the inclusive range [0, 1]. */
            Button& alpha(float value);

            /** Controls whether pressing this component can assign keyboard focus. */
            Button& focusable(bool enabled) noexcept;

            /**
             * Requests keyboard focus when this component is focusable and attached to a
             * window.
             */
            Button& setFocus() noexcept;

            /** Clears keyboard focus when this component is the current window target. */
            Button& clearFocus() noexcept;

            /** Submits the complete owning window widget tree for rendering. */
            Button& commit();

            /**
             * Moves this widget one level above its next sibling and immediately
             * repaints.
             */
            Button& raise();

            /**
             * Moves this widget one level below its previous sibling and immediately
             * repaints.
             */
            Button& sink();

            /** Moves this widget above every sibling and immediately repaints. */
            Button& raiseToTop();

            /** Moves this widget below every sibling and immediately repaints. */
            Button& sinkToBottom();

            /** Sets the UTF-8 button text. */
            Button& text(std::string_view value);

            /** Sets the text to a normalized RGBA color. */
            Button& textColor(const Color& color);

            /** Sets text height in density-independent pixels. */
            Button& fontSize(float dp);

            /** Sets the numeric font weight in the inclusive range [100, 900]. */
            Button& fontWeight(unsigned int weight);

            /** Selects a registered application font family. */
            Button& fontFamily(std::string_view familyName);

            /** Returns the selected application font family. */
            [[nodiscard]] std::string getFontFamily() const;

            /** Selects grayscale, RGB, or BGR subpixel text antialiasing. */
            Button& subpixelRendering(SubpixelRendering mode) noexcept;

            /** Returns a copy of the configured UTF-8 text. */
            [[nodiscard]] std::string getText() const;

            /** Returns the configured text color. */
            [[nodiscard]] Color getTextColor() const noexcept;

            /** Returns the configured text height in density-independent pixels. */
            [[nodiscard]] float getFontSize() const noexcept;

            /** Returns the configured numeric font weight. */
            [[nodiscard]] unsigned int getFontWeight() const noexcept;

            /** Returns the configured text antialiasing mode. */
            [[nodiscard]] SubpixelRendering getSubpixelRendering() const noexcept;

        private:
            /**
             * Starts a left-button ripple at the local click position and emits
             * ON_CLICKED.
             */
            void handleClick(MouseButton button, Position localPosition) override;

            /** Creates immutable Button-specific render properties. */
            [[nodiscard]] std::shared_ptr<const Internal::RenderPayload> renderPayload() const override;

            class Impl;
            std::unique_ptr<Impl> impl_; ///< Private Button-specific component state.
    };

#if defined(_MSC_VER) && !defined(OPENGLGUI_STATIC)
#pragma warning(pop)
#endif

/** Provides a focusable single-line UTF-8 text editor with a movable caret. */
#if defined(_MSC_VER) && !defined(OPENGLGUI_STATIC)
#pragma warning(push)
// The PImpl pointer is never accessed by value across the DLL boundary.
#pragma warning(disable : 4251)
#endif
    class OPENGLGUI_API EditText : public Widget
    {
        public:
            static constexpr int ON_TEXT_CHANGED = 11; ///< Text changed through keyboard editing.

            /**
             * Creates a focusable single-line editor optionally owned by another widget.
             */
            explicit EditText(Widget* parentWidget = nullptr);

            /** Destroys child widgets and releases retained editor content. */
            ~EditText() override;

            EditText(const EditText&) = delete;
            EditText& operator=(const EditText&) = delete;
            EditText(EditText&&) = delete;
            EditText& operator=(EditText&&) = delete;

            /**
             * Sets the position relative to the parent widget in density-independent
             * pixels.
             */
            EditText& place(Position position) noexcept;

            /** Controls whether both dimensions fill the direct parent or root window. */
            EditText& fillMaxSize(bool enabled);

            /** Controls whether the width fills the direct parent or root window width. */
            EditText& fillMaxWidth(bool enabled);

            /** Controls whether the height fills the direct parent or root window height. */
            EditText& fillMaxHeight(bool enabled);

            /** Limits the maximum dimensions applied by the active fill axes. */
            EditText& maxSize(Size dimensions);

            /** Limits the minimum dimensions applied by the active fill axes. */
            EditText& minSize(Size dimensions);

            /**
             * Controls whether this component and its descendants participate in
             * rendering.
             */
            EditText& visible(bool enabled) noexcept;

            /** Sets the component background to a normalized RGBA color. */
            EditText& backgroundColor(const Color& color);

            /** Sets the component corner radius in density-independent pixels. */
            EditText& borderRadius(float radius);

            /**
             * Sets the component border width in density-independent pixels; zero
             * disables the border.
             */
            EditText& borderWidth(float dp);

            /** Sets the component border to a normalized RGBA color. */
            EditText& borderColor(const Color& color);

            /**
             * Sets the component shadow width in density-independent pixels; zero
             * disables the shadow.
             */
            EditText& shadowWidth(float dp);

            /** Sets the component shadow to a normalized RGBA color. */
            EditText& shadowColor(const Color& color);

            /**
             * Sets the Gaussian blur radius in density-independent pixels applied to
             * everything inside the component bounds.
             */
            EditText& blurRadius(float radius);

            /** Sets the component opacity in the inclusive range [0, 1]. */
            EditText& alpha(float value);

            /** Controls whether pressing this component can assign keyboard focus. */
            EditText& focusable(bool enabled) noexcept;

            /**
             * Requests keyboard focus when this component is focusable and attached to a
             * window.
             */
            EditText& setFocus() noexcept;

            /** Clears keyboard focus when this component is the current window target. */
            EditText& clearFocus() noexcept;

            /** Submits the complete owning window widget tree for rendering. */
            EditText& commit();

            /**
             * Moves this widget one level above its next sibling and immediately
             * repaints.
             */
            EditText& raise();

            /**
             * Moves this widget one level below its previous sibling and immediately
             * repaints.
             */
            EditText& sink();

            /** Moves this widget above every sibling and immediately repaints. */
            EditText& raiseToTop();

            /** Moves this widget below every sibling and immediately repaints. */
            EditText& sinkToBottom();

            /** Sets normalized single-line UTF-8 text and moves the caret to its end. */
            EditText& text(std::string_view value);

            /** Limits stored Unicode scalars, truncating current text if needed; npos removes the limit. */
            EditText& maxCharacter(std::size_t count);

            /** Returns the stored Unicode scalar limit, or std::string::npos when unlimited. */
            [[nodiscard]] std::size_t getMaxCharacter() const noexcept;

            /** Sets text height in density-independent pixels. */
            EditText& fontSize(float dp);

            /** Sets multiline baseline spacing as a finite multiplier in [1, 4]; default 1. */
            EditText& lineSpacing(float multiplier);

            /** Returns the multiline baseline spacing multiplier. */
            [[nodiscard]] float getLineSpacing() const noexcept;

            /** Enables soft wrapping for multiline text; disabled by default. Source text is preserved. */
            EditText& wordWrap(bool enabled = true);

            /** Returns whether multiline soft wrapping is enabled. */
            [[nodiscard]] bool isWordWrap() const noexcept;

            /** Sets the numeric font weight in the inclusive range [100, 900]. */
            EditText& fontWeight(unsigned int weight);

            /** Selects a registered application font family. */
            EditText& fontFamily(std::string_view familyName);

            /** Returns the selected application font family. */
            [[nodiscard]] std::string getFontFamily() const;

            /** Sets the text and caret to a normalized RGBA color. */
            EditText& textColor(const Color& color);

            /** Selects grayscale, RGB, or BGR subpixel text antialiasing. */
            EditText& subpixelRendering(SubpixelRendering mode) noexcept;

            /**
             * Selects single-line or source-newline-delimited multiline editing
             * behavior.
             */
            EditText& multiLine(bool enabled = true);

            /** Hides both overlay scrollbars without disabling scrolling; false restores automatic display. */
            EditText& hideScrollBars(bool hidden = true);

            /** Returns whether automatic scrollbar display is disabled. */
            [[nodiscard]] bool areScrollBarsHidden() const noexcept;

            /** Returns whether the vertical scrollbar is currently shown. */
            [[nodiscard]] bool isVerticalScrollBarVisible() const noexcept;

            /** Returns whether the horizontal scrollbar is currently shown. */
            [[nodiscard]] bool isHorizontalScrollBarVisible() const noexcept;

            /** Moves the caret to a Unicode code-point boundary. */
            EditText& cursorPosition(std::size_t position) noexcept;

            /** Resizes the editor and keeps the caret inside the visible text area. */
            EditText& size(Size dimensions) override;

            /** Returns a copy of the current UTF-8 text. */
            [[nodiscard]] std::string getText() const;

            /** Returns the text height in density-independent pixels. */
            [[nodiscard]] float getFontSize() const noexcept;

            /** Returns the configured numeric font weight. */
            [[nodiscard]] unsigned int getFontWeight() const noexcept;

            /** Returns the text and caret color. */
            [[nodiscard]] Color getTextColor() const noexcept;

            /** Returns the configured text antialiasing mode. */
            [[nodiscard]] SubpixelRendering getSubpixelRendering() const noexcept;

            /** Returns whether source-newline-delimited multiline editing is enabled. */
            [[nodiscard]] bool isMultiLine() const noexcept;

            /** Returns the caret position as a Unicode code-point index. */
            [[nodiscard]] std::size_t getCursorPosition() const noexcept;

            /** Selects the complete current text. */
            EditText& selectAll() noexcept;

            /** Returns the lower Unicode code-point boundary of the current selection. */
            [[nodiscard]] std::size_t getSelectionStart() const noexcept;

            /** Returns the upper Unicode code-point boundary of the current selection. */
            [[nodiscard]] std::size_t getSelectionEnd() const noexcept;

        protected:
            /** Keeps the caret visible when the inherited border width changes. */
            void borderWidthChanged() override;

            /** Places the caret and starts a possible left-button selection drag. */
            void handlePointerPress(MouseButton button, Position localPosition) override;

            /** Extends the current selection to the nearest glyph boundary. */
            void handlePointerDrag(MouseButton button, Position localPosition) override;

            /** Scrolls multiline text vertically without changing the caret or selection. */
            bool handlePointerScroll(Position localPosition, double horizontal, double vertical) override;

            /** Ends a scrollbar gesture and preserves the inherited click event. */
            void handleClick(MouseButton button, Position localPosition) override;

            /** Applies navigation, selection, clipboard, and deletion keys. */
            virtual void handleKeyInput(int key, int modifiers) override;

            /** Inserts one Unicode scalar at the retained caret. */
            void handleCharacterInput(std::uint32_t codePoint) override;

            /** Updates caret visibility and emits the inherited focus event. */
            void handleFocusChanged(bool focused) override;

            /** Creates immutable EditText-specific render properties. */
            [[nodiscard]] std::shared_ptr<const Internal::RenderPayload> renderPayload() const override;

            class Impl;
            std::unique_ptr<Impl> impl_; ///< Private editable text, caret, font,
                                         ///< selection, and scroll state.
    };

#if defined(_MSC_VER) && !defined(OPENGLGUI_STATIC)
#pragma warning(pop)
#endif
#if defined(_MSC_VER) && !defined(OPENGLGUI_STATIC)
#pragma warning(push)
// The PImpl pointer is never accessed by value across the DLL boundary.
#pragma warning(disable : 4251)
#endif
    /** Provides a focusable single-line UTF-8 editor with a movable caret and completer. */
    class OPENGLGUI_API LineEdit : public EditText
    {
        public:
            /** Creates a single-line editor with an optional owning widget. */
            explicit LineEdit(Widget* parentWidget = nullptr);

            /** Destroys the editor and its completer popup entries. */
            ~LineEdit() override;

            LineEdit(const LineEdit&) = delete;
            LineEdit& operator=(const LineEdit&) = delete;
            LineEdit(LineEdit&&) = delete;
            LineEdit& operator=(LineEdit&&) = delete;

            /** Sets single-line placeholder text, shown only when empty at half the text color alpha. */
            LineEdit& hintText(std::string_view text);

            /** Replaces the completer candidate source. */
            LineEdit& setCompleter(const std::vector<std::string>& values);

            /** Sets the maximum number of visible candidates; five by default. */
            LineEdit& completerMaximumVisibleItems(std::size_t count);

            /** Commits the widget and requests its default initial focus. */
            LineEdit& commit();

        protected:
            /** Adds display-only placeholder text without modifying editing or selection state. */
            [[nodiscard]] std::shared_ptr<const Internal::RenderPayload> renderPayload() const override;

        private:
            /** Creates the independent native completion window on demand. */
            void ensureCompleterWindow();

            /** Rebuilds the visible prefix-matching completer entries. */
            void refreshCompleter();

            /** Writes the highlighted candidate on Enter and dismisses the popup until further input. */
            void acceptCompletion();

            /** Hides the completer after focus leaves the editor. */
            void handleFocusLost(const EventData& message);

            /** Hides the popup immediately and defers native destruction to the event pump. */
            void closeCompleter();

            std::string hintText_{};                      ///< Placeholder retained separately from editable text.
            MainWindow* completerWindow_{};               ///< Independent native popup window.
            bool completerShown_{};                       ///< Whether the popup has been shown.
            std::vector<std::string> completerValues_{};  ///< Candidate strings.
            std::vector<std::string> completerMatches_{}; ///< Filtered candidates backing the visible popup.
            std::string completerPrefix_{};               ///< Text used for filtering and detecting subsequent edits.
            bool completerDismissed_{};            ///< Suppresses reopening accepted text on deferred repaint events.
            std::vector<Label*> completerItems_{}; ///< Popup labels owned by this editor.
            std::size_t completerMaximumVisibleItems_{5U}; ///< Visible candidate limit.
            std::size_t completerSelection_{};             ///< Highlighted candidate index.
    };
#if defined(_MSC_VER) && !defined(OPENGLGUI_STATIC)
#pragma warning(pop)
#endif
#if defined(_MSC_VER) && !defined(OPENGLGUI_STATIC)
#pragma warning(push)
// The PImpl pointer is never accessed by value across the DLL boundary.
#pragma warning(disable : 4251)
#endif
    class OPENGLGUI_API ScrollArea : public Widget
    {
        public:
            /** Creates a scroll area optionally owned by another widget. */
            explicit ScrollArea(Widget* parentWidget = nullptr);

            /** Unsubscribes from the content component before children are destroyed. */
            ~ScrollArea() override;

            ScrollArea(const ScrollArea&) = delete;
            ScrollArea& operator=(const ScrollArea&) = delete;
            ScrollArea(ScrollArea&&) = delete;
            ScrollArea& operator=(ScrollArea&&) = delete;

            /**
             * Sets the position relative to the parent widget in density-independent
             * pixels.
             */
            ScrollArea& place(Position position) noexcept;

            /** Resizes the viewport and immediately recomputes the scrollbars. */
            ScrollArea& size(Size dimensions) override;

            /** Controls whether both dimensions fill the direct parent or root window. */
            ScrollArea& fillMaxSize(bool enabled);

            /** Controls whether the width fills the direct parent or root window width. */
            ScrollArea& fillMaxWidth(bool enabled);

            /** Controls whether the height fills the direct parent or root window height. */
            ScrollArea& fillMaxHeight(bool enabled);

            /** Limits the maximum dimensions applied by the active fill axes. */
            ScrollArea& maxSize(Size dimensions);

            /** Limits the minimum dimensions applied by the active fill axes. */
            ScrollArea& minSize(Size dimensions);

            /**
             * Controls whether this component and its descendants participate in
             * rendering.
             */
            ScrollArea& visible(bool enabled) noexcept;

            /** Sets the component background to a normalized RGBA color. */
            ScrollArea& backgroundColor(const Color& color);

            /** Sets the component corner radius in density-independent pixels. */
            ScrollArea& borderRadius(float radius);

            /**
             * Sets the component border width in density-independent pixels; zero
             * disables the border.
             */
            ScrollArea& borderWidth(float dp);

            /** Sets the component border to a normalized RGBA color. */
            ScrollArea& borderColor(const Color& color);

            /**
             * Sets the component shadow width in density-independent pixels; zero
             * disables the shadow.
             */
            ScrollArea& shadowWidth(float dp);

            /** Sets the component shadow to a normalized RGBA color. */
            ScrollArea& shadowColor(const Color& color);

            /**
             * Sets the Gaussian blur radius in density-independent pixels applied to
             * everything inside the component bounds.
             */
            ScrollArea& blurRadius(float radius);

            /** Sets the component opacity in the inclusive range [0, 1]. */
            ScrollArea& alpha(float value);

            /** Controls whether pressing this component can assign keyboard focus. */
            ScrollArea& focusable(bool enabled) noexcept;

            /**
             * Requests keyboard focus when this component is focusable and attached to a
             * window.
             */
            ScrollArea& setFocus() noexcept;

            /** Clears keyboard focus when this component is the current window target. */
            ScrollArea& clearFocus() noexcept;

            /** Submits the complete owning window widget tree for rendering. */
            ScrollArea& commit();

            /**
             * Moves this widget one level above its next sibling and immediately
             * repaints.
             */
            ScrollArea& raise();

            /**
             * Moves this widget one level below its previous sibling and immediately
             * repaints.
             */
            ScrollArea& sink();

            /** Moves this widget above every sibling and immediately repaints. */
            ScrollArea& raiseToTop();

            /** Moves this widget below every sibling and immediately repaints. */
            ScrollArea& sinkToBottom();

            /**
             * Adopts one content component scrolled by this viewport, replacing and
             * destroying any previous content; ownership transfers to this scroll
             * area.
             */
            ScrollArea& setWidget(Widget* widget);

            /**
             * Scrolls the viewport to a clamped density-independent content offset and
             * immediately repaints when the offset changed.
             */
            ScrollArea& scrollTo(Position offset);

            /** Hides both overlay scrollbars without disabling scrolling; false restores automatic display. */
            ScrollArea& hideScrollBars(bool hidden = true);

            /** Returns whether automatic scrollbar display is disabled. */
            [[nodiscard]] bool areScrollBarsHidden() const noexcept;

            /** Returns the adopted content component, or nullptr when empty. */
            [[nodiscard]] Widget* widget() const noexcept;

            /** Returns the current clamped density-independent content offset. */
            [[nodiscard]] Position getScrollOffset() const noexcept;

            /** Returns whether the vertical scrollbar is currently shown. */
            [[nodiscard]] bool isVerticalScrollBarVisible() const noexcept;

            /** Returns whether the horizontal scrollbar is currently shown. */
            [[nodiscard]] bool isHorizontalScrollBarVisible() const noexcept;

        protected:
            /** Scrolls the viewport when the mouse wheel moves over this component. */
            bool handlePointerScroll(Position localPosition, double horizontalOffset, double verticalOffset) override;

        private:
            class Impl;
            std::unique_ptr<Impl> impl_; ///< Private content, offset, and scrollbar
                                         ///< state.
    };
#if defined(_MSC_VER) && !defined(OPENGLGUI_STATIC)
#pragma warning(pop)
#endif
} // namespace OpenGLGUI
