Bugfix: Follow same site redirects in the Wizard

We fixed a bug where the client did not follow same site redirects during the setup.
If the used URL https://test.com/owncloud redirected to https://test.com the new URL was not correctly used for the newly created account.

https://github.com/owncloud/enterprise/issues/5743
