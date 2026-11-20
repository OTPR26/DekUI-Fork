# SPDX-License-Identifier: GPL-3.0-or-later
option(REMASKED_PUBLIC_MOBILE "Build a mobile runtime without bundled game code" OFF)

function(remasked_reject_bundled_public_title)
  if(REMASKED_PUBLIC_MOBILE)
    message(FATAL_ERROR
      "This mobile target links a game module into the app. Public ReMasked builds "
      "require a runtime that runs player-supplied game code from local storage.")
  endif()
endfunction()
