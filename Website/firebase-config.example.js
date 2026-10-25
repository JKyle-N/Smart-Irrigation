/*
 * Copy this file to firebase-config.js, then paste the Firebase Web App
 * configuration from Firebase Console > Project settings > Your apps > Web app.
 * This value identifies the project; database access is protected by Firebase
 * Authentication and Realtime Database Rules, not by hiding this configuration.
 */
window.FIREBASE_CONFIG = {
  apiKey: "PASTE_YOUR_API_KEY",
  authDomain: "YOUR_PROJECT.firebaseapp.com",
  databaseURL: "https://YOUR_DATABASE_NAME.REGION.firebasedatabase.app",
  projectId: "YOUR_PROJECT_ID",
  storageBucket: "YOUR_PROJECT_ID.firebasestorage.app",
  messagingSenderId: "YOUR_SENDER_ID",
  appId: "YOUR_APP_ID"
};

// Endpoint for the operator-only "Delete user" action (see delete-user-api/) -- deploy that
// project to Vercel/Netlify first, then paste its URL here. Not a secret; the privileged
// credentials live only in that service's own environment variables, never in this file.
window.DELETE_USER_API_URL = "https://YOUR-PROJECT.vercel.app/api/delete-user";
