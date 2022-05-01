- On keyboard, pressing a button (A/B/C/D) for less than a frame will now hold that button for one frame, to prevent
  dropping quickly pressed inputs
- New config option `patches.skip_legal` (default `true`) lets you skip the legal screen on startup
- New config options `patches.default_username` and `patches.default_password` let you set, well, the default username
  and password so you don't have to enter it manually. Note that the default password only works if a username is set,
  and only for that username.

  ```
  patches {
  	default_username BOB
  	default_password ABCDAB
  }
  ```
