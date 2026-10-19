# Publishing the dashboard — GitHub + Vercel

## 1. Push to GitHub

From the project folder:

```bash
git init
git add .
git commit -m "Synaptix PDM: ESP8266 firmware, pre-trained model, dashboard"
git branch -M main
git remote add origin https://github.com/<your-username>/pdm-motor-ml.git
git push -u origin main
```

(Create the empty repo on github.com first — no README, no .gitignore, so the
push is not rejected.)

## 2. Deploy to Vercel

1. Go to **vercel.com → Add New → Project** and import the repo.
2. Framework preset: **Other**.
3. Leave Build Command empty. `vercel.json` already sets
   `"outputDirectory": "dashboard"`, so Vercel serves the dashboard folder as a
   static site.
4. Deploy. You get `https://<project>.vercel.app`.

Or from the CLI:

```bash
npm i -g vercel
vercel --prod
```

## 3. The one thing that will bite you: HTTPS → HTTP

Vercel serves your dashboard over **HTTPS**. The ESP8266 serves its API over
**HTTP**. Browsers block that combination (mixed content), so the live Vercel
page **cannot** poll your ESP8266 by default. The dashboard detects this and
shows a banner instead of failing silently.

Three honest ways to handle it, pick one:

| Option | What you do | Good for |
|---|---|---|
| **A — Local copy** *(recommended for the live demo)* | Run the dashboard over plain HTTP on your laptop: `npx serve dashboard` then open `http://localhost:3000`. Same files, same repo. | Lab demo, viva, video |
| **B — Allow insecure content** | On the Vercel URL: Chrome → padlock → Site settings → *Insecure content* → **Allow**, then reload. | Showing the real deployed URL |
| **C — Demo mode** | Click **Demo mode** on the Vercel page. Synthetic telemetry, fault-injection buttons, no hardware. | Sharing the link with anyone |

The Vercel deployment is still worth having: it is the public, shareable face of
the project and Demo mode makes it fully interactive for anyone who opens it.

## 4. Optional: serve the dashboard from the ESP8266 itself

If you want one URL with no mixed-content issue at all, host the page on the
board with LittleFS:

1. Install the **ESP8266 LittleFS Data Upload** plugin for the Arduino IDE.
2. Create `firmware/data/` and copy `index.html`, `styles.css`, `app.js` into it.
3. Add to the firmware:
   ```cpp
   #include <LittleFS.h>
   LittleFS.begin();
   server.serveStatic("/", LittleFS, "/index.html");
   server.serveStatic("/styles.css", LittleFS, "/styles.css");
   server.serveStatic("/app.js", LittleFS, "/app.js");
   ```
4. Set the dashboard's device IP field to blank — same-origin requests just work.

The 4 MB flash on a NodeMCU has plenty of room for these three files.
