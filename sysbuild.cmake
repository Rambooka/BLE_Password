# Official dongle only: package MCUboot (and nothing else) for the factory
# Nordic Open USB bootloader -> <build>/dongle_mcuboot_migration.zip.
#
# One-time migration from the old non-MCUboot firmware.  The Nordic bootloader
# CRC-checks everything it flashed on every power-on, so it must only ever
# carry MCUboot (which never changes); if the app were included, the first BLE
# update (a slot swap) would break that CRC and strand the dongle in Nordic DFU.
# The first app image is then loaded through MCUboot's USB serial recovery.
if(SB_CONFIG_BOOTLOADER_MCUBOOT AND BOARD MATCHES "^nrf52840dongle")
  find_program(NRFUTIL nrfutil)
  if(NRFUTIL)
    set(mcuboot_hex ${CMAKE_BINARY_DIR}/mcuboot/zephyr/zephyr.hex)
    set(migration_pkg ${CMAKE_BINARY_DIR}/dongle_mcuboot_migration.zip)
    add_custom_command(
      OUTPUT ${migration_pkg}
      COMMAND ${NRFUTIL} pkg generate
        --hw-version 52
        --sd-req 0x00
        --application ${mcuboot_hex}
        --application-version 1
        ${migration_pkg}
      DEPENDS mcuboot ${mcuboot_hex}
      COMMENT "Generating Nordic-bootloader migration package (MCUboot only)"
      VERBATIM
    )
    add_custom_target(dongle_mcuboot_migration ALL DEPENDS ${migration_pkg})
  else()
    message(WARNING "nrfutil not found -- dongle MCUboot migration package not generated")
  endif()
endif()
