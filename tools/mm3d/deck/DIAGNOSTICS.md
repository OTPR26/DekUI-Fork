# Opt-in performance captures

Normal builds hide these controls and do not automatically start a capture.
Set this Steam launch option for the native Linux AppImage (no Proton):

```text
DEKUI_DIAGNOSTICS=1 %command%
```

Or launch from a terminal beside the AppImage:

```sh
DEKUI_DIAGNOSTICS=1 ./Majora-ReMasked.AppImage
```

Open F1, select Diagnostics, then Capture performance (30 seconds). Close F1
within the three-second delay and reproduce the slowdown for 30 seconds.
The build revision and graphics settings are included in the log. Record the
scene separately. Compare Original and CPU interpolation at the same scene,
camera angle and resolution. Keep settings unchanged during each capture.

The tab displays the log directory. After the captures, quit normally and copy
`azahar_log.txt` before relaunching, which rotates the log. Portable packages use
`user/log/azahar_log.txt` beside the AppImage. Several captures can share one log;
they have session identifiers. Wait for each capture to finish before starting
another. Changing interpolation enablement cancels an active capture.

The report contains CPU scopes, replay batching, waits and GPU submission timing
where timestamps are supported. Scopes may overlap; submission timing includes
GPU waits and does not measure utilization or display latency. Capturing itself
adds overhead. Use matching captures for comparisons. Nothing is uploaded.

Remove the launch option and restart to hide the tab again. This option is not
saved in user settings. It does not enable the developer editor or change the
rendering algorithm. It is a support switch, not a password or access control.
