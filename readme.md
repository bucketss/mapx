just drag and drop archives (7z rar zip) and it'll extract the files, put them in the proper `cstrike\` subfolders, then deletes or sequesters the junk (like readmes and screenshots and configs)

needs [7-Zip](https://www.7-zip.org/) installed (`7z` on PATH or default install )

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

this doesn't just extract, it will also check the bsp and put loose files in the correct locations
