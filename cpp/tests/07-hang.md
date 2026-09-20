# Runaway page script

The script below never returns. Chrome stays alive and responsive at the process
level but its renderer main thread is wedged, so `Runtime.evaluate` never
answers. This is the case that used to look like "Chrome hangs": md2pdf waited
on a reply that could not arrive.

<script>while (true) { /* spin the renderer forever */ }</script>

This paragraph is never reached.
