# Interacting with other groups

As we move forward, one group may find that for their module, they need something not currently provided from another group, or that something provided from another group needs to be changed.

There are a few options:

- **The group that needs the change makes the change.**  This is the best option when the needed change is small, or time is of the essence.  However, the change must not be pushed to the 'main' branch of the target repository, but bundled as a Pull Request.  The requesting group can then use the CI-built binaries to continue their own work.
The target repository's owner can then review and accept, modify, or reject that PR, but if they modify or reject, they must work with the requesting group to find a new way to provide what is needed going forward.

- **The group that needs the change adds an issue.** The requesting group should include:
  - What timeline they need the change by.
  - What specific change is needed.
  - If at all possible, test data and expected outputs.
  
  The owning group should reply within a day with any questions, and an estimated timeline for the change.

- **The group that needs the change discusses the change in person.**  This is the best option when it's unclear what the best path forward is, and can be faster than filing an issue in some cases.  Once the solution is clear, someone then needs to file an issue on the issue tracker, both to keep track of the request, and to note when the request has been fulfilled.
