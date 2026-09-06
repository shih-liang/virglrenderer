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
MoltenVK and epoxy package metadata. The final application must link the
MoltenVK archive; do not enable ``vulkan-dload`` for this configuration, since
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

The backend requests hardware sessions only. Guest capabilities currently
include H.264 decode/encode, HEVC Main/Main10 encode, and JPEG/VP9 decode when
VideoToolbox reports hardware support. HEVC and AV1 decode are not advertised:
the stateless guest protocol does not carry all the original parameter sets
required by VideoToolbox. Direct backend callers supplying complete bitstreams
can use the retained HEVC/AV1 paths; this is not general guest VA-API support.
Capability dimensions/levels are backend ceilings, not per-device
measurements; creating the hardware session remains authoritative.

Decode commands accumulate until ``END_FRAME`` and produce one picture.
Compatible parameter-set changes preserve the decoder session and its
reference pictures. Completion callbacks are synchronous and return errors to
the VGL context. Encoder controls currently cover profile, bitrate, frame rate
and keyframes, not every Gallium rate-control/reference-picture option.

This implementation is not zero-copy YUV: CVPixelBuffer planes are uploaded to
guest GL textures on decode and read back on encode. Sharing IOSurface Metal
planes with the renderer, with explicit resource ownership and synchronization,
is a separate optimization. Codec work also completes synchronously; callers
must not interpret the hardware path as an asynchronous throughput guarantee.

The focused macOS checks use existing static archives and require no guest,
downloads, libcheck or VM restart:

.. code-block:: sh

  bash tests/run_videotoolbox.sh /path/to/vgl-build /path/to/angle-prefix /path/to/libMoltenVK.a

They check framing and frame lifetime, hardware encode/decode round trips,
NV12/P010 transfers and GL state restoration, and the public VGL video command
and error paths. ASan/UBSan instrument the test translation units (which include
the backend and transfer implementation); prebuilt dependency archives are not
instrumented. These checks do not substitute for guest VA-API/application tests.


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
