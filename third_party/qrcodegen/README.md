# QR Code generator (Project Nayuki)

`qrcodegen.hpp` / `qrcodegen.cpp` are the C++ edition of Project Nayuki's QR
Code generator library, vendored verbatim from
<https://github.com/nayuki/QR-Code-generator/tree/master/cpp> (MIT License, see
the file headers). Aurora uses it only through `src/util/QrCode.hpp` for the
"Share as QR code" dialog. Compiled into `aurora_core` with warnings disabled,
because it is third-party code.
