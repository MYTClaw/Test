import os
import time
import base64
import smtplib
from email.mime.text import MIMEText
from email.mime.multipart import MIMEMultipart
from email.mime.application import MIMEApplication
from msal import ConfidentialClientApplication
from dotenv import load_dotenv
from pathlib import Path

load_dotenv()

TENANT_ID = os.getenv("O365_TENANT_ID")
CLIENT_ID = os.getenv("O365_CLIENT_ID")
CLIENT_SECRET = os.getenv("O365_CLIENT_SECRET")
SMTP_USERNAME = os.getenv("SMTP_USERNAME")
SMTP_HOST = "smtp.office365.com"
SMTP_PORT = 587


def get_access_token():
    authority = f"https://login.microsoftonline.com/{TENANT_ID}"
    app = ConfidentialClientApplication(
        CLIENT_ID,
        authority=authority,
        client_credential=CLIENT_SECRET
    )

    scopes = ["https://outlook.office365.com/.default"]

    result = app.acquire_token_for_client(scopes=scopes)

    if "access_token" in result:
        return result["access_token"]
    else:
        raise Exception(f"Could not get access token: {result.get('error_description')}")


def generate_oauth2_string(username, access_token):
    auth_string = f"user={username}\x01auth=Bearer {access_token}\x01\x01"
    return base64.b64encode(auth_string.encode()).decode()


def send_email(to_addresses, subject, body_text, body_html=None, attachments=None):
    access_token = get_access_token()
    auth_string = generate_oauth2_string(SMTP_USERNAME, access_token)

    msg = MIMEMultipart("mixed")
    msg["Subject"] = subject
    msg["From"] = SMTP_USERNAME
    msg["To"] = ", ".join(to_addresses)

    # Create alternative part for text and HTML
    alt_part = MIMEMultipart("alternative")
    alt_part.attach(MIMEText(body_text, "plain", "utf-8"))
    if body_html:
        alt_part.attach(MIMEText(body_html, "html", "utf-8"))

    msg.attach(alt_part)

    # Attach files
    if attachments:
        for file_path in attachments:
            path = Path(file_path)
            if not path.is_file():
                raise Exception(f"Attachment not found: {file_path}")

            with open(path, "rb") as f:
                part = MIMEApplication(f.read(), Name=path.name)
                part['Content-Disposition'] = f'attachment; filename="{path.name}"'
                msg.attach(part)

    with smtplib.SMTP(SMTP_HOST, SMTP_PORT) as smtp:
        smtp.ehlo()
        smtp.starttls()
        smtp.ehlo()
        smtp.docmd("AUTH", "XOAUTH2 " + auth_string)
        smtp.sendmail(SMTP_USERNAME, to_addresses, msg.as_string())
        print("Email sent successfully.")


if __name__ == "__main__":
    send_email(
        to_addresses=["othniel.n@difinative.com"],
        subject="DevOps- ALert Test",
        body_text="This is a test-alert.",
        body_html="<h1>This is a test alert</h1><p>With <b>test</b> message!</p>",
    )
