//! Opt-in S3b.3 cut/fill parity driver, never linked into the shipped archive.
//!
//! `terrain_volume <file>` reads whitespace-separated tokens:
//!
//! ```text
//! existing <vertices> <triangles> <x y z ...> <a b c ...>
//! proposed mesh <vertices> <triangles> <x y z ...> <a b c ...>   | proposed plane <z>
//! grid <cell> <subdivisions>
//! ```
//!
//! and prints one JSON line with the site volumes, sample counts and cells, so an
//! external oracle can compare identical whole-terrain grids.
#![forbid(unsafe_code)]

use std::process::ExitCode;
use tapioca_geometry::terrain::field::Field;
use tapioca_geometry::terrain::volume::{Grid, Proposed, VolumeRequest, cut_fill};

struct Tokens<'a>(std::str::SplitWhitespace<'a>);

impl Tokens<'_> {
    fn word(&mut self) -> Result<&str, String> {
        self.0
            .next()
            .ok_or_else(|| "unexpected end of input".to_owned())
    }
    fn expect(&mut self, word: &str) -> Result<(), String> {
        let got = self.word()?;
        if got == word {
            Ok(())
        } else {
            Err(format!("expected {word}, got {got}"))
        }
    }
    fn number<T: std::str::FromStr>(&mut self) -> Result<T, String> {
        let word = self.word()?;
        word.parse().map_err(|_| format!("not a number: {word}"))
    }
    fn mesh(&mut self) -> Result<(Vec<f64>, Vec<u32>), String> {
        let vertices: usize = self.number()?;
        let triangles: usize = self.number()?;
        let points = (0..vertices * 3)
            .map(|_| self.number())
            .collect::<Result<_, _>>()?;
        let indices = (0..triangles * 3)
            .map(|_| self.number())
            .collect::<Result<_, _>>()?;
        Ok((points, indices))
    }
}

fn run() -> Result<String, String> {
    let path = std::env::args()
        .nth(1)
        .ok_or("usage: terrain_volume <file>")?;
    let text = std::fs::read_to_string(&path).map_err(|error| format!("{path}: {error}"))?;
    let mut tokens = Tokens(text.split_whitespace());
    tokens.expect("existing")?;
    let (points, triangles) = tokens.mesh()?;
    let existing = Field::new(&points, &triangles).map_err(|error| error.message().to_owned())?;
    tokens.expect("proposed")?;
    let other;
    let proposed = match tokens.word()? {
        "mesh" => {
            let (points, triangles) = tokens.mesh()?;
            other = Field::new(&points, &triangles).map_err(|error| error.message().to_owned())?;
            Proposed::Field(&other)
        }
        "plane" => Proposed::Plane(tokens.number()?),
        kind => return Err(format!("unknown proposed kind {kind}")),
    };
    tokens.expect("grid")?;
    let grid = Grid {
        cell: tokens.number()?,
        subdivisions: tokens.number()?,
    };
    let report = cut_fill(&VolumeRequest {
        existing: &existing,
        proposed,
        grid,
        domain: None,
        property: None,
    })
    .map_err(|error| error.message().to_owned())?;
    let site = report.site;
    Ok(format!(
        "{{\"cutVolume\":{:.17e},\"fillVolume\":{:.17e},\"netVolume\":{:.17e},\"analysedCells\":{},\"excludedCells\":{},\"cells\":[{},{}]}}",
        site.cut,
        site.fill,
        site.net,
        site.samples,
        site.excluded,
        report.cells[0],
        report.cells[1]
    ))
}

fn main() -> ExitCode {
    match run() {
        Ok(line) => {
            println!("{line}");
            ExitCode::SUCCESS
        }
        Err(error) => {
            eprintln!("{error}");
            ExitCode::FAILURE
        }
    }
}
