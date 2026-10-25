window.FIREBASE_CONFIG = {
  apiKey: "AIzaSyDWUeD3kTUX6RBwOhP-ZKAP4qQznQu6n_M",
  authDomain: "smart-irrigation-web2.firebaseapp.com",
  databaseURL: "https://smart-irrigation-web2-default-rtdb.asia-southeast1.firebasedatabase.app",
  projectId: "smart-irrigation-web2",
  storageBucket: "smart-irrigation-web2.firebasestorage.app",
  messagingSenderId: "90035435913",
  appId: "1:90035435913:web:c514dc8fd2d80e8cb84958",
  measurementId: "G-140GK8P1MD"
};

// Not a secret -- just an endpoint address (the actual privileged credentials live only in that
// service's own environment variables, never here). Fill in once delete-user-api/ is deployed to
// Vercel/Netlify; see the User Management "Delete" action in script.js for where this is used.
window.DELETE_USER_API_URL = "https://delete-user-api-xi.vercel.app/api/delete-user";
