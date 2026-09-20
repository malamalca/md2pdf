# Image handling

## Plain relative path

![gradient](media/gradient.png)

## Path containing spaces

CommonMark forbids bare spaces in a link destination, so the path has to be
wrapped in angle brackets. Both the folder and the file name below contain a
space; before the fix the resulting `file:///` URL was emitted unencoded and
Chrome silently refused to load it.

![gradient](<media folder/test image.png>)

## Pipe syntax: width

![](media/gradient.png|30%)

## Pipe syntax: width and centring

![](media/gradient.png|40%|center)

## Pipe syntax: float right

![](media/gradient.png|20%|float:right)

Text that should wrap alongside the floated image. Lorem ipsum dolor sit amet,
consectetur adipiscing elit, sed do eiusmod tempor incididunt ut labore et
dolore magna aliqua. Ut enim ad minim veniam, quis nostrud exercitation ullamco
laboris nisi ut aliquip ex ea commodo consequat.

## Inline data URI

![dot](data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAoAAAAKCAYAAACNMs+9AAAAHElEQVQoz2NgGAWjYBSMglEwCkbBKBgFo2AUAAAHhAABkTz0UgAAAABJRU5ErkJggg==)

A data URI must be passed through untouched.

## CSS background, path with spaces

<div style="width:200px;height:60px;background:url('media folder/test image.png');border:1px solid #999"></div>
