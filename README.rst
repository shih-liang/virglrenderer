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
