// Palette tokens shared with 247-steam/web/style.css and Asset-Extractor/src/theme.rs.
#[derive(Clone, Copy)]
pub struct Palette {
    pub background: u32,
    pub surface: u32,
    pub accent: u32,
    pub hover: u32,
    pub primary: u32,
    pub primary_hover: u32,
    pub ring: u32,
    pub muted: u32,
    pub border: u32,
}
pub const NAMES: [&str; 10] = [
    "Amethyst",
    "Cobalt",
    "Ruby",
    "Graphite",
    "Evergreen",
    "Lagoon",
    "Amber",
    "Rose",
    "Ember",
    "Midnight",
];
pub fn palette(index: usize) -> Palette {
    let v = match index {
        1 => [
            0x233047, 0x1c273b, 0x2059ac, 0x2b6ac5, 0x2586f5, 0x4b9fff, 0x91c9ff, 0xcbbfd0,
            0x73667b,
        ],
        2 => [
            0x3e262d, 0x321e25, 0xa52a47, 0xbb3655, 0xe54b70, 0xf76587, 0xffb3c3, 0xcbbfd0,
            0x73667b,
        ],
        3 => [
            0x2e2b31, 0x242127, 0x55515e, 0x686473, 0x77717f, 0x8b8494, 0xddd5e6, 0xcbbfd0,
            0x73667b,
        ],
        4 => [
            0x22382e, 0x1a2d24, 0x245b40, 0x2c6c4d, 0x16804f, 0x198353, 0x8ce0b0, 0xbfd0c5,
            0x5e7c6b,
        ],
        5 => [
            0x203638, 0x192b2e, 0x205b60, 0x286c71, 0x147c80, 0x148184, 0x8cdee0, 0xbdd0d1,
            0x5c7b7e,
        ],
        6 => [
            0x383023, 0x2d261b, 0x6c4c17, 0x805b20, 0x956300, 0xa36b0c, 0xf5cf83, 0xd4cbb8,
            0x807052,
        ],
        7 => [
            0x3b2632, 0x301e29, 0x80284e, 0x94325d, 0xbe356e, 0xcd407b, 0xf6a5c9, 0xd5c0cb,
            0x856275,
        ],
        8 => [
            0x3b2c24, 0x30221b, 0x823b1b, 0x964721, 0xb94e18, 0xbf521b, 0xffb58b, 0xd6c5ba,
            0x856b5c,
        ],
        9 => [
            0x00192e, 0x09243b, 0x254d85, 0x1c4268, 0x285bcc, 0x3772fe, 0x7ca7ff, 0x9ca3b8,
            0x254d85,
        ],
        _ => [
            0x35263e, 0x2e2037, 0x6515ad, 0x7924c5, 0xa617f5, 0xb637ff, 0xd69dff, 0xcbbfd0,
            0x73667b,
        ],
    };
    Palette {
        background: v[0],
        surface: v[1],
        accent: v[2],
        hover: v[3],
        primary: v[4],
        primary_hover: v[5],
        ring: v[6],
        muted: v[7],
        border: v[8],
    }
}
