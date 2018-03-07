# Telegram Bitlbee Plugin

This plugin is based on Telegram's official client library
[tdlib](https://github.com/tdlib/td). *(This is a proof of concept, not
intended for public consumption!)*

## Dependencies

- [tdjson](https://github.com/tdlib/td)
- [jansson](https://github.com/akheron/jansson)
- [glib](https://github.com/GNOME/glib)
- [bitlbee](https://github.com/bitlbee/bitlbee)

## Build

- Put your API keys in `tgx.h` (see:
  https://core.telegram.org/api/obtaining_api_id).
- Run `make`.
- Install the generated `telegram.so` library into Bitlbee's plugins directory.

## Usage

1. Run `bitlbee` from an interactive terminal.
2. [Bitlbee IM-account maintenance](https://www.bitlbee.org/user-guide.html#cmd_account).

## Limitations

- This is a PoC.
- Supports only one client.
- Authentication information is read from stdin.
- Only basic chats are implemented.
- Parsing JSON requests in C is cumbersome.
