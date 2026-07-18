ever download a bunch of maps, then go to unzip them and they're all in different directory structures?

so instead of just unzipping and playing them, you have to untangle the mess while also deleting a bunch of useless readmes and screenshots >:(

I hate that shit lmao, so here's a program for it.

just drag and drop archives (7z rar zip) and it'll extract the files, put them in the proper `cstrike\` subfolders, then deletes or sequesters the junk (like readmes and screenshots and configs)

```
flags:
-l, --log       write timestamped log file
-j, --junk      delete junk files (default: save to junk folder)
-a, --addons    ignore addons\ directory (default: save to __addons\)
-e, --executable extract to exe dir (default: archive dir)
-d, --delete    delete archives after successful extraction
-h, --help      show help message
```

by default it moves junk files to a junk folder, and also preserves any `addons\` folder content in `__addons\` to prevent overwrites (but still allow a map to come with an accompanying plugin or w/e)

this doesn't just extract, it will also put loose files in the correct locations
