# Install rules and the .deb (DEVPLAN 4.2).
#
#   cmake --preset deb && cmake --build --preset deb && cpack --preset deb
#
# The deb preset installs to /usr, so the package has /usr/bin/meerkat,
# /etc/meerkat/config.toml (a conffile), the unit in
# /usr/lib/systemd/system, the sysusers file and the polkit rule.
# packaging/install.sh is the other way, without a package (/usr/local).

# The service files only for a package (the deb preset): a plain
# `cmake --install` puts just the binary in place, and install.sh sets the
# service up from the source tree (#116).
option(MEERKAT_INSTALL_SERVICE "Install the systemd unit, user, polkit rule and /etc config (for the .deb)" OFF)
if(NOT MEERKAT_INSTALL_SERVICE)
  return()
endif()

# The unit names the binary and the config: the package's paths, not
# install.sh's.
set(meerkat_exec_line "ExecStart=/usr/local/bin/meerkat run --config /etc/meerkat/config.toml")
file(READ "${PROJECT_SOURCE_DIR}/packaging/meerkat.service" meerkat_unit)
string(FIND "${meerkat_unit}" "${meerkat_exec_line}" meerkat_exec_at)
if(meerkat_exec_at EQUAL -1)
  message(FATAL_ERROR "packaging/meerkat.service: ExecStart= isn't the line cmake/Package.cmake rewrites")
endif()
string(REPLACE "${meerkat_exec_line}"
       "ExecStart=${CMAKE_INSTALL_FULL_BINDIR}/meerkat run --config ${CMAKE_INSTALL_FULL_SYSCONFDIR}/meerkat/config.toml"
       meerkat_unit_out "${meerkat_unit}")
file(WRITE "${PROJECT_BINARY_DIR}/packaging/meerkat.service" "${meerkat_unit_out}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/packaging/meerkat.service")

# 0755 whatever the umask of the build (a package's folders aren't group-writable).
set(CMAKE_INSTALL_DEFAULT_DIRECTORY_PERMISSIONS
    OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)

install(FILES "${PROJECT_BINARY_DIR}/packaging/meerkat.service" DESTINATION lib/systemd/system)
install(FILES packaging/meerkat.sysusers DESTINATION lib/sysusers.d RENAME meerkat.conf)
install(FILES packaging/50-meerkat.rules DESTINATION ${CMAKE_INSTALL_DATADIR}/polkit-1/rules.d)
install(FILES config.example.toml DESTINATION ${CMAKE_INSTALL_FULL_SYSCONFDIR}/meerkat RENAME config.toml)
install(FILES README.md THIRD_PARTY_LICENSES.md config.example.toml packaging/debian/copyright
        DESTINATION ${CMAKE_INSTALL_DOCDIR})

# --- CPack: .deb -----------------------------------------------------------------
set(CPACK_GENERATOR DEB)
set(CPACK_PACKAGE_NAME meerkat)
set(CPACK_PACKAGE_VENDOR "meerkat")
set(CPACK_PACKAGE_CONTACT "carloslinares-eu <53991568+carloslinares-eu@users.noreply.github.com>")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Internet speed monitor that records evidence of a slow line")
# CPack puts the summary first as the synopsis by itself.
set(CPACK_DEBIAN_PACKAGE_DESCRIPTION "Runs a speed test on a schedule and connectivity probes every 30 s,
records every result with its context (cable or Wi-Fi, server, time)
and writes a report against the contract's speeds under the
Bundesnetzagentur rules. Needs the Ookla Speedtest CLI, which isn't
part of this package.")
set(CPACK_PACKAGE_HOMEPAGE_URL "${PROJECT_HOMEPAGE_URL}")
set(CPACK_RESOURCE_FILE_LICENSE "${PROJECT_SOURCE_DIR}/LICENSE")
set(CPACK_PACKAGING_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")
set(CPACK_STRIP_FILES ON)
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_SECTION net)
set(CPACK_DEBIAN_PACKAGE_PRIORITY optional)
# libc6, libsystemd0, zlib1g from the binary; ping and systemd-sysusers by name.
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_DEPENDS "iputils-ping, systemd")
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "iw, polkitd")
set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
    "${PROJECT_SOURCE_DIR}/packaging/debian/postinst;${PROJECT_SOURCE_DIR}/packaging/debian/prerm;${PROJECT_SOURCE_DIR}/packaging/debian/postrm;${PROJECT_SOURCE_DIR}/packaging/debian/conffiles")
set(CPACK_DEBIAN_PACKAGE_CONTROL_STRICT_PERMISSION ON)
include(CPack)
