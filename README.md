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

If the library has native plugins, ellib lists them and asks for confirmation first (add `-y` to skip the question), because a plugin is a program that runs with your permissions.

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

## Plugins

A library can also have native plugins (a `.dll`, `.so` or `.dylib`) that add new functions to El, for example for MySQL, hashing, or talking to another system. List them in `plugins`, one file for each system:

```json
{
  "name": "hashing",
  "version": "1.0.0",
  "plugins": ["hashing.dll", "hashing.so"]
}
```

`files` can be left out when a library is only a plugin. ellib installs only the plugin file for your system, and elvm loads it when the program says `library "hashing";`.

The header for writing a plugin is `sdk/elplugin.h`, and `library/hashing/` is a small working example with its source in `library/hashing/src/`. For the full guide, see Plugins and Writing a Plugin in the El Language docs.

Every plugin file listed in `plugins` has to exist in the repository, so build the plugin for each system you list and commit the result.

## Installation

Ellib is automaticly including in elvm setup, download in https://github.com/DelfinIbnKadafi/EL-Language

## Publish your library to official repository

Fork this repository first, and make a subfolder in library folder, and make your library in that folder. And then create a pull request in here.

## License

MIT License

Free to update and use.
