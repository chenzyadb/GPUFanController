# Third-party notices

OpenGLGUI incorporates GLFW when it is built through the default CMake configuration.

## GLFW

The source in `third_party/glfw` is an altered GLFW 3.4 distribution maintained
by OpenGLGUI. It includes the Win32 transparency fix from GLFW PR #2815 and
an adaptation of the layered-window workaround discussed in GLFW PR #2681.
See `third_party/glfw/OPENGLGUI_FORK.md` for upstream revisions and local changes;
the original license is preserved in `third_party/glfw/LICENSE.md`.

Copyright (c) 2002-2006 Marcus Geelnard

Copyright (c) 2006-2019 Camilla Löwy

This software is provided 'as-is', without any express or implied
warranty. In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not
   claim that you wrote the original software. If you use this software
   in a product, an acknowledgment in the product documentation would
   be appreciated but is not required.

2. Altered source versions must be plainly marked as such, and must not
   be misrepresented as being the original software.

3. This notice may not be removed or altered from any source
   distribution.

## stb_image and stb_truetype

Copyright (c) 2017 Sean Barrett

OpenGLGUI contains tailored internal copies derived from the stb single-header
libraries. They are dual-licensed under the MIT License or public domain. The
complete license text remains in `src/image/StbImage.hpp` and
`src/text/StbTrueType.hpp`.

## nlohmann/json (Mellow Notes example)

The standalone example downloads or links nlohmann/json 3.12.0 under the MIT
License. Its source and license are retained in the example build's dependency
directory; it is not a dependency of the OpenGLGUI library itself.

Copyright (c) 2013-2025 Niels Lohmann
