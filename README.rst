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

How to use
----------

.. code-block:: sh

  $ virgl_test_server --venus --no-fork --multi-clients \
                      --use-egl-surfaceless --use-gles \
                      --socket-path=/data/share/tmp/.virgl_test

Performance
-----------

LLVM host:

.. code-block:: sh

  $ glxgears -info
  GL_RENDERER   = llvmpipe (LLVM 21.1.8, 128 bits)
  GL_VERSION    = 4.5 (Compatibility Profile) Mesa 25.3.2
  GL_VENDOR     = Mesa
  GL_EXTENSIONS = ...
  VisualID 979, 0x3d3
  958 frames in 5.0 seconds = 191.490 FPS
  983 frames in 5.0 seconds = 196.580 FPS

ZINK host:

.. code-block:: sh

  $ glxgears -info
  GL_RENDERER   = zink Vulkan 1.1(Mali-G76 (Driver Unknown))
  GL_VERSION    = 2.1 Mesa 24.3.0-devel (git-c1567e9609)
  GL_VENDOR     = Mesa
  GL_EXTENSIONS = ...
  VisualID 979, 0x3d3
  1263 frames in 5.0 seconds = 252.593 FPS
  1163 frames in 5.0 seconds = 232.317 FPS

virgl host:

.. code-block:: sh

  $ glxgears -info
  GL_RENDERER   = virgl (Mali-G76)
  GL_VERSION    = 4.5 (Compatibility Profile) Mesa 25.3.2
  GL_VENDOR     = Mesa
  GL_EXTENSIONS = ...
  VisualID 979, 0x3d3
  780 frames in 5.0 seconds = 155.832 FPS
  732 frames in 5.0 seconds = 146.298 FPS

virgl guest(lxc):

.. code-block:: sh

  $ glxgears -info
  GL_RENDERER   = virgl (Mali-G76)
  GL_VERSION    = 3.2 (Compatibility Profile) Mesa 25.0.7-0ubuntu0.25.04.2
  GL_VENDOR     = Mesa
  GL_EXTENSIONS = ...
  VisualID 979, 0x3d3
  629 frames in 5.0 seconds = 125.653 FPS
  604 frames in 5.0 seconds = 120.691 FPS

Venus
-----
.. code-block:: sh

  $ vulkaninfo --summary
  Devices:
  ========
  GPU0:
          apiVersion         = 1.1.82
          driverVersion      = 104857607
          vendorID           = 0x13b5
          deviceID           = 0x72110000
          deviceType         = PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU
          deviceName         = Virtio-GPU Venus (Mali-G76)

fastfetch
---------

.. code-block:: sh

  root@ubuntu:~# fastfetch
                               ....              root@ubuntu
                .',:clooo:  .:looooo:.           -----------
             .;looooooooc  .oooooooooo'          OS: Ubuntu 25.04 aarch64
          .;looooool:,''.  :ooooooooooc          Host: kirin980
         ;looool;.         'oooooooooo,          Kernel: Linux 4.9.148-DreamConnected-All-in-One+
        ;clool'             .cooooooc.  ,,       Uptime: 1 day, 16 hours, 38 mins
           ...                ......  .:oo,      Packages: 2384 (dpkg)
    .;clol:,.                        .loooo'     Shell: bash 5.2.37
   :ooooooooo,                        'ooool     Display (builtin): 1080x2310 @ 60 Hz
  'ooooooooooo.                        loooo.    Theme: Breeze [GTK2/3]
  'ooooooooool                         coooo.    Icons: breeze [GTK2/3/4]
   ,loooooooc.                        .loooo.    Font: Noto Sans (10pt) [GTK2/3/4]
     .,;;;'.                          ;ooooc     Cursor: breeze (24px)
         ...                         ,ooool.     Terminal: vt220
      .cooooc.              ..',,'.  .cooo.      CPU: kirin980 (8) @ 2.60 GHz
        ;ooooo:.           ;oooooooc.  :l.       GPU: Virtio-GPU Venus (Mali-G76) [Integrated]
         .coooooc,..      coooooooooo.           Memory: 3.82 GiB / 5.53 GiB (69%)
           .:ooooooolc:. .ooooooooooo'           Swap: 53.91 MiB / 6.00 GiB (1%)
             .':loooooo;  ,oooooooooc            Disk (/): 39.44 GiB / 108.22 GiB (36%) - f2fs
                 ..';::c'  .;loooo:'             Local IP (eth0): 10.0.3.11/24
                                                 Battery: 95% [Charging, AC Connected]
                                                 Locale: en_US.UTF-8
                                                 
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
