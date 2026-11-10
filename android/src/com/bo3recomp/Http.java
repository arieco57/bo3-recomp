package com.bo3recomp;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;

// HTTPS for the native cloud sync (see runtime/src/http.cpp): returns 4 bytes of big-endian status
// followed by the response body; status 0 = failure (the body is then the error text).
public class Http {
    public static byte[] request(String method, String url, String[] headers, byte[] body, int timeoutMs) {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        int status = 0;
        byte[] payload;
        try {
            HttpURLConnection c = (HttpURLConnection) new URL(url).openConnection();
            c.setConnectTimeout(timeoutMs);
            c.setReadTimeout(timeoutMs);
            if (method.equals("PATCH")) {   // HttpURLConnection lacks PATCH: PostgREST accepts the override header
                c.setRequestMethod("POST");
                c.setRequestProperty("X-HTTP-Method-Override", "PATCH");
            } else {
                c.setRequestMethod(method);
            }
            for (int i = 0; i + 1 < headers.length; i += 2) c.setRequestProperty(headers[i], headers[i + 1]);
            if (!method.equals("GET") && body != null) {
                c.setDoOutput(true);
                OutputStream os = c.getOutputStream();
                os.write(body);
                os.close();
            }
            status = c.getResponseCode();
            InputStream is = status >= 400 ? c.getErrorStream() : c.getInputStream();
            ByteArrayOutputStream b = new ByteArrayOutputStream();
            if (is != null) {
                byte[] buf = new byte[16384];
                int n;
                while ((n = is.read(buf)) > 0) b.write(buf, 0, n);
                is.close();
            }
            payload = b.toByteArray();
            c.disconnect();
        } catch (Exception e) {
            status = 0;
            payload = String.valueOf(e).getBytes();
        }
        out.write((status >> 24) & 255);
        out.write((status >> 16) & 255);
        out.write((status >> 8) & 255);
        out.write(status & 255);
        out.write(payload, 0, payload.length);
        return out.toByteArray();
    }
}
