# EL Library

El Libary (Ellib) is a package/libary manager for El Language.

## Install Library

Install from official repository. The official repository is https://github.com/DelfinIbnKadafi/El-Library
Official packages must be placed at `library/<library_name>/library.json`.

```bash
ellib install lib_name
```

Install from another github repository.

```bash
ellib install -g username/repository_name
```

## Uninstall Libary

```bash
ellib uninstall lib_name
```

## How It Works?

When you install new libary, ellib will find `library.json` in the repository, if the format is correct,
ellib will install all of file in the repository and put all of them in `{Elvm Path}/library/{library names}`.

And when you call/use the libary in the code, like :

```el
library "lib_name";
```

Elvm will find the folder and read the json file.

*library.json*

```json
{
  "name": "Name",
  "description": "Descriptions",
  "version": "x.x.x",
  "author": "Author Name",
  "files": ["files1.ell", "files2.ell"]
}
```

And elvm will put all  of code in the main code like modules.

## Installation

Ellib is automaticly including in elvm setup, download in https://github.com/DelfinIbnKadafi/EL-Language

## Publish your library to official repository

Fork this repository first, and make a subfolder in library folder, and make your plugin in that folder. And then create a pull request in here.

## License

MIT License

Free to update and use.
