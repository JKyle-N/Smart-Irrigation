const admin = require("firebase-admin");

// Reads the service-account key and database URL from Vercel's own environment-variable secret
// store -- set these in the Vercel project dashboard, never in a file. FIREBASE_PRIVATE_KEY comes
// out of the downloaded service-account JSON with literal "\n" sequences that env vars store as
// text, not real newlines -- the replace() below restores them before handing it to the SDK.
if (!admin.apps.length) {
  admin.initializeApp({
    credential: admin.credential.cert({
      projectId: process.env.FIREBASE_PROJECT_ID,
      clientEmail: process.env.FIREBASE_CLIENT_EMAIL,
      privateKey: (process.env.FIREBASE_PRIVATE_KEY || "").replace(/\\n/g, "\n"),
    }),
    databaseURL: process.env.FIREBASE_DATABASE_URL,
  });
}

// Same literal-UID pattern as Website/firebase-rules.json -- compared against the uid Firebase
// itself resolves from a verified ID token below, never a value the caller supplies directly.
// Two operators: primary (espoperator32@gmail.com) and a hidden backup/creator account
// (johnmichaelsugian123@gmail.com) -- both authorized identically, neither can delete the other.
const OPERATOR_UIDS = ["KaiqrwlOHeVdebbokPxZfcFLZiu2", "C8V39t4EDgUgjOt9GA6OeMukMsw2"];
const ESP1_DEVICE_UID = "6hNg56ldAaSmpnodG1Xla0vn61O2";

/* The one privileged operation nothing in Website/ can do client-side: deleting another user's
 * Firebase Auth identity. admin.auth().deleteUser() only exists here, where the Admin SDK's
 * service-account credentials live (as Vercel environment variables, never in a served file).
 * A Firebase callable Cloud Function would verify the caller automatically (context.auth); a
 * plain Vercel function has no such integration, so verifyIdToken() does that check by hand --
 * same verification, just explicit instead of implicit. Also removes /users/{uid} in the same
 * trusted call, so both deletions happen together. */
module.exports = async (req, res) => {
  res.setHeader("Access-Control-Allow-Origin", process.env.ALLOWED_ORIGIN || "");
  res.setHeader("Access-Control-Allow-Headers", "Authorization, Content-Type");
  res.setHeader("Access-Control-Allow-Methods", "POST, OPTIONS");
  if (req.method === "OPTIONS") { res.status(204).end(); return; }
  if (req.method !== "POST") { res.status(405).json({ error: "Method not allowed" }); return; }

  const authHeader = req.headers.authorization || "";
  const idToken = authHeader.startsWith("Bearer ") ? authHeader.slice(7) : "";
  if (!idToken) { res.status(401).json({ error: "Missing bearer token" }); return; }

  let decoded;
  try {
    // New-F11 (audit): checkRevoked=true so a token captured before an operator's tokens were
    // revoked (e.g. after a compromise) stops working here immediately, not just at its own expiry.
    decoded = await admin.auth().verifyIdToken(idToken, true);
  } catch (error) {
    // Audit fix (2026-09-08): this branch is reachable by ANY caller, not just an authenticated
    // non-operator -- verifyIdToken() can throw before the operator check below ever runs, so a
    // fully unauthenticated request (garbage/expired/malformed token) previously got the raw Admin
    // SDK error string back (e.g. "Firebase ID token has invalid signature"). Not a secret, but
    // unnecessary fingerprinting/probing surface for an unauthenticated caller -- log the real
    // reason server-side only, return a generic message here.
    console.error("verifyIdToken failed:", error);
    res.status(401).json({ error: "Invalid or expired token" });
    return;
  }

  if (!OPERATOR_UIDS.includes(decoded.uid)) {
    res.status(403).json({ error: "Only the operator can delete accounts." });
    return;
  }

  const targetUid = typeof req.body?.uid === "string" ? req.body.uid : "";
  if (!targetUid) { res.status(400).json({ error: "A target uid is required." }); return; }
  if (OPERATOR_UIDS.includes(targetUid)) { res.status(400).json({ error: "Operator accounts cannot be deleted through this action." }); return; }
  if (targetUid === ESP1_DEVICE_UID) { res.status(400).json({ error: "Cannot delete ESP1's own device account." }); return; }

  // Reached only by a verified operator (checked above), so a detailed error here is not an
  // unauthenticated-facing information leak the way the token-verification branch above was.
  try {
    await admin.auth().deleteUser(targetUid);
    // Deep-scan finding (2026-09-10): this used to remove only /users/{uid} -- /presence/{uid} and
    // /activity/{uid} (added this session) are SEPARATE top-level nodes, not children of /users, so
    // they were silently left behind forever after a delete. Harmless as dead data (neither is ever
    // displayed again once the uid drops out of /users, which is what every reader keys off), but
    // cleaned up here anyway now that this call already has the Admin SDK's rules-bypassing access
    // to do it properly in the same trusted operation.
    // Deep-scan finding (2026-09-10): admin.auth().deleteUser() above already committed by this
    // point, so Promise.all() was the wrong tool here -- one transient failure among the three
    // removes would reject the whole batch and report 500 "delete failed" even though the Auth
    // account was already gone, and a retry's own deleteUser() call would then immediately throw
    // "no such user" before ever reaching cleanup again, permanently orphaning whichever node didn't
    // get removed the first time. allSettled() lets each removal succeed or fail independently and
    // still reports success for the part that actually matters (the Auth account is gone), while
    // surfacing which cleanup path failed for a manual follow-up instead of silently orphaning it.
    const cleanup = await Promise.allSettled([
      admin.database().ref(`users/${targetUid}`).remove(),
      admin.database().ref(`presence/${targetUid}`).remove(),
      admin.database().ref(`activity/${targetUid}`).remove()
    ]);
    const failedPaths = ["users", "presence", "activity"].filter((_, i) => cleanup[i].status === "rejected");
    if (failedPaths.length) console.error(`Cleanup failed for: ${failedPaths.join(", ")}`, cleanup);
    res.status(200).json({ success: true, cleanupFailed: failedPaths.length ? failedPaths : undefined });
  } catch (error) {
    console.error(error);
    res.status(500).json({ error: error.message || "Unknown error" });
  }
};
