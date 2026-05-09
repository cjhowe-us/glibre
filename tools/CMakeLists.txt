cmake_minimum_required(VERSION 3.30)

# ---------------------------------------------------------------------------
# tools/ — glibre host-side tools
#
# Each tool ships as its own subdirectory. Tools are built for the host
# machine (not cross-compiled) and are never linked into shipping dylibs.
#
# New tools: add add_subdirectory(<name>) here.
# ---------------------------------------------------------------------------

add_subdirectory(foryc)
