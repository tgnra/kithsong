Mozilla CA certificate bundle in PEM format, from https://curl.se/ca/cacert.pem.
Licensed under MPL-2.0 (see the header of cacert.pem).

It is embedded into the core at build time (`cmake/EmbedFile.cmake`) and passed to curl
with `CURLOPT_CAINFO_BLOB`, so HTTPS never depends on the OS trust store. Refresh it
periodically by downloading the file again.
