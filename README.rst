`Virglrenderer <https://virgil3d.github.io/>`_ - The VirGL virtual OpenGL renderer
==================================================================================


Source
------

This repository lives at https://gitlab.freedesktop.org/virgl/virglrenderer.
Other repositories are likely forks, and code found there is not supported.


Build & install
---------------

This project uses the meson build system:

.. code-block:: sh

  $ meson build
  $ cd build
  $ ninja install


macOS sandbox hosts
-------------------

When linking this renderer with static MoltenVK, configure with
``-Ddefault_library=static -Dvenus=true -Dvulkan-dload=false
-Drender-server-worker=thread``. Point ``-Dpkg_config_path`` at the supplied
MoltenVK and ANGLE package metadata (``vulkan.pc`` and ``angle.pc``).
The ANGLE SDK must include its EGL/GLES headers plus the Khronos
``GL/glcorearb.h`` and ``GL/glext.h`` format declarations. The renderer
calls ANGLE's EGL/GLES symbols directly; no GL dispatch library is needed.
The final application must link both static archives; do not enable
``vulkan-dload`` for this configuration, since
that tries to open an external ``libvulkan.dylib`` at runtime instead.

Before initializing the renderer, set ``XDG_RUNTIME_DIR`` to the application's
private temporary directory (``FileManager.temporaryDirectory`` in an App
Sandbox host). The macOS eventfd emulation and anonymous shared files use that
directory, or ``TMPDIR`` when it is unset. They unlink their directory entries
after creation; the descriptors, including copies sent over ``SCM_RIGHTS``,
remain valid for their normal lifetime.


macOS video
-----------

Configure with ``-Dvideo=true`` and pass ``VIRGL_RENDERER_USE_VIDEO`` to
``virgl_renderer_init``. A DRM fd callback is not required on macOS. Static
consumers must link VideoToolbox, CoreMedia, CoreVideo and IOSurface in addition
to their existing ANGLE/Metal dependencies.

The video wire enums match Mesa 26.2.2, which sends Gallium profile and chroma
numbers without a version negotiation. Earlier Mesa releases used different
numbers; compatibility with those video drivers is not implied. The public
command test uses literal guest wire values as well as checking returned caps.

The backend requests hardware sessions only. Guest capabilities currently
include H.264 decode/encode, HEVC Main/Main10 decode/encode, and JPEG/VP9 decode
when VideoToolbox reports hardware support. With missing HEVC parameter sets,
the backend writes equivalent VPS/SPS/PPS and explicit slice reference sets
from the resolved descriptor. It preserves compressed slice payloads rather
than re-encoding pictures, and retains original parameter sets when supplied.

AV1 Main 8/10-bit descriptor-to-OBU reconstruction is implemented and tested
with independent software parsing/decoding, including tiled and film-grain
streams. AV1 decode is not advertised to guests yet: the VideoToolbox path
still needs hidden-frame output validation on AV1-capable hardware, separate
reference/display target delivery for film grain, and reference-slot updates
for show-existing key frames that bypass guest decode submission. A distinct
film-grain target is rejected, not silently populated with the wrong picture.
Capability dimensions/levels are backend ceilings, not per-device
measurements; creating the hardware session remains authoritative.

Decode commands accumulate until ``END_FRAME`` and produce one picture.
Compatible parameter-set changes preserve the decoder session and its
reference pictures. VideoToolbox decode is asynchronous; its completion owns
the target storage independently of guest video wrappers. The host
``END_FRAME`` handler waits for decode and fixed-plane writes before returning;
failures return a command error rather than reporting successful completion.
Encoder controls currently cover profile, bitrate, frame rate
and keyframes, not every Gallium rate-control/reference-picture option.

Decoded CVPixelBuffer planes are copied by Metal into the guest's fixed native
video resources; encode performs the reverse copy. The transfer module has no
GL or Vulkan dependency, does not map YUV data on the CPU, and retains CoreVideo
views until GPU completion. This is one GPU copy, not zero-copy decoding.
NV12/P010 use direct blits; I420/YV12 use a blit for Y and a compute dispatch
to split/join UV into the fixed planar resources, without an intermediate image.
The VirGL adapter orders native writes after prior GL work using a shared event.
There are no video-specific ANGLE/Vulkan consumer waits or additions to VirGL
submission fences. Ordinary guest-requested submission fences are created after
``END_FRAME`` returns and retain their normal batch-completion semantics.
Queue reservations preserve native copy order even when decoder callbacks
arrive out of order.
The decoder and native transfer module never call GL/EGL; that dependency is
confined to the VirGL adapter's prior-work dependency.
Guest video resources still use the renderer's existing allocation/import path.
Encoding still waits for its input copy before handing pixels to VideoToolbox.

Host completion does not make the guest's asynchronous submission ioctl wait.
Mesa 26.2.2's VirGL decoder does not retain a per-picture submission fence for
``vaSyncSurface``. An independent Vulkan consumer can therefore read an older
frame even after that API returns success. Decoder fences were added in Mesa
`MR !20133 <https://gitlab.freedesktop.org/mesa/mesa/-/merge_requests/20133>`_
and disabled following a transcoding regression in
`MR !21145 <https://gitlab.freedesktop.org/mesa/mesa/-/merge_requests/21145>`_.
This renderer does not modify Mesa or install a synchronization workaround.
Mesa 26.2.2's VirGL video path also does not dirty newly decoded plane resources
for CPU readback: its clean-mask optimization can return stale guest storage.
DMA-BUF imports access the native storage, but still require correct producer
completion. Successful VA API calls or CPU transfer return codes alone are not
pixel proof; host tests do not establish end-to-end guest synchronization.

The focused macOS checks use existing static archives and require no guest,
downloads, libcheck or VM restart:

.. code-block:: sh

  bash tests/run_videotoolbox.sh /path/to/vgl-build /path/to/angle-prefix /path/to/libMoltenVK.a
  bash tests/run_gl_direct.sh /path/to/vgl-build /path/to/angle-prefix

They check framing and frame lifetime, hardware encode/decode round trips,
NV12/P010 native transfers, delayed completion and resource lifetime, and the public VGL video command
and error paths. The direct-GL check verifies extension entry points in a GLES
3.0 context, including indexed color masks and debug callbacks.
ASan/UBSan instrument the test translation units (which include
the backend and transfer implementation); prebuilt dependency archives are not
instrumented. These checks do not substitute for guest VA-API/application tests.

Header reconstruction has a separate corpus test. It uses test-only FFmpeg and
dav1d development libraries and pre-existing ``hevc-*.mp4`` / ``av1-*.ivf``
streams; neither library is a runtime dependency of the renderer:

.. code-block:: sh

  bash tests/run_video_bitstream.sh /path/to/vgl-build /path/to/test-media

Its acceptance checks cover decoded header semantics, reference relationships,
unchanged compressed payloads, dimensions/bit depth and error handling.
Same-decoder pixel equality is reported only as a diagnostic. Differences must
be evaluated against the applicable codec's normative process and conformance
bounds; no blanket byte-exact hardware/software requirement or arbitrary pixel
tolerance is imposed. AV1 hardware checks report a skip when unavailable,
not a successful decode.


Support
-------

Many Virglrenderer devs hang on IRC; if you're not sure which channel is
appropriate, you should ask your question on `OFTC's #virgil3d
<irc://irc.oftc.net/virgil3d>`_, someone will redirect you if
necessary.
Remember that not everyone is in the same timezone as you, so it might
take a while before someone qualified sees your question.

The next best option is to ask your question in an email to the
mailing lists: `virglrenderer-devel\@lists.freedesktop.org
<https://lists.freedesktop.org/mailman/listinfo/virglrenderer-devel>`_


Bug reports
-----------

If you think something isn't working properly, please file a bug report in
`GitLab <https://gitlab.freedesktop.org/virgl/virglrenderer/-/issues>`_.


Contributing
------------

Contributions are welcome, note that Virglrenderer uses GitLab for patches
submission, review and discussions.
