Running Weston
==============

libweston uses the concept of a *back-end* to abstract the interface to the
underlying environment where it runs on. Ultimately, the back-end is
responsible for handling the input and generate an output. Weston, as a
libweston user, can be run on different back-ends, including nested, by using
the wayland backend, but also on X11 or on a stand-alone back-end like
DRM/KMS and now deprecated fbdev.

In most cases, people should allow Weston to choose the backend automatically
as it will produce the best results. That happens for instance when running
Weston on a machine that already has another graphical environment running,
being either another wayland compositor (e.g.  Weston) or on a X11 server.
You should only specify the backend manually if you know that what Weston picks
is not the best, or the one you intended to use is different than the one
loaded.  In that case, the backend can be selected by using ``-B [backend.so]``
command line option.  As each back-end uses a different way to get input and
produce output, it means that the most suitable back-end depends on the
environment being used.

Available back-ends:

* **drm** -- run stand-alone on DRM/KMS and evdev (recommend)
  (`DRM kernel doc <https://www.kernel.org/doc/html/latest/gpu/index.html>`_)
* **wayland** -- run as a Wayland application, nested in another Wayland compositor
  instance
* **x11** -- run as a x11 application, nested in a X11 display server instance
* **rdp** -- run as an RDP server without local input or output
* **headless** -- run without input or output, useful for test suite
* **fbdev** -- run stand-alone on fbdev/evdev (deprecated)

The job of gathering all the surfaces (windows) being displayed on an output and
stitching them together is performed by a *renderer*. By doing so, it is
compositing all surfaces into a single image, which is being handed out to a
back-end, and finally, displayed on the screen.

libweston has a CPU-based type of renderer by making use of the
`Pixman <http://www.pixman.org/>`_ library, but also one that can make
use of the GPU to do that, which uses `OpenGL ES <https://www.khronos.org/opengles/>`_
and it is simply called the GL-renderer.

Most of the back-ends provide a command line option to disable the GL-renderer,
and use the CPU for doing that. That happens by appending to the command line
``--use-pixman`` when running Weston. One might use the CPU-based renderer
to exclude any other potential issues with the GL-renderer.

Additional set-up steps
-----------------------

Depending on your distribution some additional set-up parts might be required,
before actually launching Weston, although any fairly modern distribution
should have it already set-up for you. Weston creates its unix socket file (for
example, wayland-1) in the directory specified by the required
environment variable ``$XDG_RUNTIME_DIR``. Clients use the same variable to
find that socket. Normally this should already be provided by systemd.  If you
are using a distribution that does not set-up ``$XDG_RUNTIME_DIR``, you
must set it using your shell profile capability. More info about how to
set-up that up, which depends to some extent on your shell, can be found at
`Building/Running Weston <https://wayland.freedesktop.org/building.html>`_

Running Weston in a graphical environment
-----------------------------------------

As stated previously, if you are already in a graphical environment, Weston
would infer and attempt to load up the correct back-end.  Either running
in a Wayland compositor instance, or a X11 server, you should be able to run
Weston from a X terminal or a Wayland one.

Running Weston on a stand-alone back-end
----------------------------------------

Now that we are aware of the concept of a back-end and a renderer, it is time to
introduce the concept of a seat, as stand-alone back-ends require one.  A *seat*
is a collection of input devices like a keyboard and a mouse, and output
devices (monitors), forming the work or entertainment place for one person. It
could also include sound cards or cameras.  A single computer could be serving
multiple seats.

.. note::

        A graphics card is **required** to be a part of the seat, as among
        other things, it effectively drives the monitor.

By default Weston will use the default seat named ``seat0``, but there's an
option to specify which seat Weston must use by passing ``--seat`` argument.

You can start Weston from a VT assuming that there's a seat manager supported by
`libseat <https://sr.ht/~kennylevinsen/seatd>`_ running, such as ``seatd`` or
`logind <https://www.freedesktop.org/wiki/Software/systemd/logind/>`_.  The
backend to be used by ``libseat`` can optionally be selected with
``$LIBSEAT_BACKEND``.  If ``libseat`` and ``seatd`` are both installed, but
``seatd`` is not already running, it can be started with ``sudo -- seatd -g
video``.  If no seat manager supported by ``libseat`` is available, you can use
the ``weston-launch`` application that can handle VT switching.

Another way of launching Weston is via ssh or a serial terminal.  The simplest
option here is to use the ``libseat`` launcher with ``seatd``.  The process for
setting that up is identical to the one described above, where one just need to
ensure that ``seatd`` is running with the appropriate arguments, after which one
can just run ``weston``.  Alternatively and as a last resort, one can run Weston
as root, specifying the tty to use on the command line: If TTY 2 is active, one
would run ``weston --tty 2`` as root.

Running Weston on a different seat on a stand-alone back-end
------------------------------------------------------------

While Weston can be tested on top of an already running Wayland compositor or
an X11 server, another option might be to have an unused GPU card which can
be solely used by Weston.  So, instead of having a dedicated machine to run
Weston for trying out the DRM-backend, by just having an extra GPU, one can
create a new seat that could access the unused GPU on the same machine (and
potentialy other inputs) and assign it to that seat. All of the
happening while you already have your graphical environment running.

In order to have that set-up, the requirements/steps would be:

* have an extra GPU card -- you could also use integrated GPUs, while your
  other GPU is in use by another graphical environment
* create a udev file that assigns the card (and inputs) to another seat
* start Weston on that seat

Start by creating a udev file, under ``/etc/udev/rules.d/`` adding something
similar to the following:

::

        ACTION=="remove", GOTO="id_insecure_seat_end"

        SUBSYSTEM=="drm", KERNEL=="card*", KERNELS=="0000:00:02.0", ENV{ID_SEAT}="seat-insecure"

        SUBSYSTEM=="input", ATTRS{idVendor}=="222a", ATTRS{idProduct}=="004d", OWNER="your_user_id", ENV{ID_SEAT}="seat-insecure", ENV{WL_OUTPUT}="HDMI-A-1"
        SUBSYSTEM=="input", ATTRS{idVendor}=="03f0", ATTRS{idProduct}=="1198", OWNER="your_user_id", ENV{ID_SEAT}="seat-insecure"

        LABEL="id_insecure_seat_end"

By using the above udev file, devices assigned to that particular seat
will be skipped by your normal display environment. Follow the naming scheme
when creating the file (``man 7 udev``). For instance you could use
``63-insecure-seat.rules`` as a filename, but take note that other udev rules
might also be present and could potentially affect the way in which they get
applied. Check that no other rules might take precedence before adding
this new one.

.. warning::

        This seat uses on purpose the name ``seat-insecure``, to warn users
        that the input devices can be eavesdropped. Futher more, if you attempt
        doing this on a VT, without being already in a graphical environment
        (and although the udev rules do apply), there will be nothing stopping
        the events from input devices reaching the virtual terminal.

In the example above, there are two input devices, one of which is a
touch panel that is being assigned to a specific output (`HDMI-A-1`) and
another input which a mouse.  Notice how ``ENV{ID_SEAT}`` and
``ENV{WL_OUTPUT}`` specify the name of the seat, respectively the input that
should be assign to a specific output.

Resolving or extracting the udev key/value pair names, can be easily done with
the help of ``udevadm`` command, for instance issuing ``udevadm info -a
/dev/dri/cardX`` would give you the entire list of key values names for that
particular card.  Archaically, one would might also use ``lsusb`` and ``lspci``
commands to retrieve the PCI vendor and device codes associated with it.

If there are no input devices the DRM-backend can be started by appending
``--continue-without-input`` or by editing ``weston.ini`` and adding to the
``core`` section ``require-input=false``.

Then, weston can be run by selecting the DRM-backend and the seat ``seat-insecure``:

::

        ./weston -Bdrm-backend.so --seat=seat-insecure

If everything went well you should see weston be up-and-running on an output
connected to that DRM device.
