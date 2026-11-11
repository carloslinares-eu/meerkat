# meerkat_set_warnings(<target>)
# Strict warnings for meerkat's own code (third-party targets are SYSTEM).
function(meerkat_set_warnings target)
  target_compile_options(${target} PRIVATE
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
    -Wnon-virtual-dtor -Wold-style-cast -Woverloaded-virtual
    $<$<CONFIG:Debug>:-Werror>)
endfunction()
