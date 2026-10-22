# Emulator display keyboard ownership

The main guest display is an image, not an ImGui navigation widget. While that
display owns input, SPACE/arrows must reach the emulated keyboard without also
activating debugger controls. Explicit application shortcuts remain available.

Two frontend details matter:

- The image window uses `ImGuiWindowFlags_NoNavInputs`, preventing navigation
  activation and arrow movement while ImGui focus is on the display.
- Before processing a main SDL window key/focus event, reconcile a stale ImGui
  navigation target in another viewport back to the display. Native window
  focus may have returned while ImGui still remembers the debugger Pause button.

Events originating from a debugger SDL window retain debugger navigation.
Same-viewport dialogs, open popups and the overlay retain their existing input.
No keyboard navigation is disabled globally, no guest bytes are rewritten and
no CPU/device clock is changed. The implementation is renderer independent.

`emulator-input-focus` is a hidden SDL dummy/ImGui regression. It models a stale
detached viewport ID, then queues main-window SPACE and each arrow+SPACE pair.
The original behavior fails all five guest cases; the fix passes. Genuine
debugger SPACE, same-viewport UI and overlay activation remain enabled. Native
focus, unrelated events and popup ownership are checked separately. This host
fixture is not physical keyboard, OS focus or complete application certification.

Run the software-only test group without creating an OpenGL context:

```
ctest --test-dir <build> -R "^emulator-input-focus$" --output-on-failure
```
