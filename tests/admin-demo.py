#!/usr/bin/env python3
"""MovieBackend Admin Console (Tkinter)

Talks to the real admin REST API (see AdminController.hpp) to cover :

  * rename a movie                          PUT  /api/v1/admin/movies/{id}
  * rename a theater                        PUT  /api/v1/admin/theaters/{id}
  * append a movie's default schedule       POST /api/v1/admin/movies/{movieId}
    to a theater                                 /theaters/{theaterId}/default-schedule

"Append to theater" deliberately uses the *default-schedule* endpoint rather
than the single-slot endpoint, because default-schedule picks its own times
(09:00, then each following whole hour) — there is no start_time to type in.
That is also why there is no time/hour field anywhere in this tool: it was
left out on purpose, not forgotten. See the TODO block near the bottom of
this file for what a full admin console would still need.

Dependency urllib/json/tkinter, so this runs on a bare Python 3.8+ with nothing to
install beyond the stdlib's tkinter.

Usage:
    python3 admin_console.py
"""

import json
import tkinter as tk
import urllib.error
import urllib.request
from tkinter import messagebox, ttk

DEFAULT_BASE_URL = "http://localhost:8000"


# ---------------------------------------------------------------------------
# API client
# ---------------------------------------------------------------------------

class ApiError(Exception):
    """Raised for both transport failures and non-2xx HTTP responses."""

    def __init__(self, message, status=None):
        super().__init__(message)
        self.status = status


class ApiClient:
    """Thin synchronous wrapper around the admin REST API.

    Synchronous on purpose for now: every call blocks the Tk main loop for
    the duration of the request. Fine for a local server and a handful of
    entities; see the TODO block at the bottom of this file about moving
    this onto a worker thread before pointing it at anything slower.
    """

    def __init__(self, base_url):
        self.base_url = base_url.rstrip("/")

    def _request(self, method, path, payload=None):
        url = self.base_url + path
        data = None
        headers = {"Accept": "application/json"}
        if payload is not None:
            data = json.dumps(payload).encode("utf-8")
            headers["Content-Type"] = "application/json"

        request = urllib.request.Request(url, data=data, headers=headers, method=method)
        try:
            with urllib.request.urlopen(request, timeout=10) as response:
                raw = response.read().decode("utf-8")
                return json.loads(raw) if raw else None
        except urllib.error.HTTPError as error:
            raw = error.read().decode("utf-8")
            try:
                body = json.loads(raw)
                message = body.get("message", raw) if isinstance(body, dict) else raw
            except ValueError:
                message = raw or error.reason
            raise ApiError(message, status=error.code) from error
        except urllib.error.URLError as error:
            raise ApiError(str(error.reason)) from error

    # -- movies --------------------------------------------------------
    def list_movies(self):
        return self._request("GET", "/api/v1/admin/movies")

    def rename_movie(self, movie):
        """PUT the movie back with only `title` changed.

        The API's MovieRequestDto has no PATCH semantics — it replaces the
        whole record — so duration_minutes/language/genre are sent back
        unchanged alongside the new title.
        """
        body = {
            "title": movie["title"],
            "duration_minutes": movie["duration_minutes"],
            "language": movie.get("language"),
            "genre": movie.get("genre"),
        }
        return self._request("PUT", "/api/v1/admin/movies/{}".format(movie["id"]), body)

    # -- theaters --------------------------------------------------------
    def list_theaters(self):
        return self._request("GET", "/api/v1/admin/theaters")

    def rename_theater(self, theater):
        """Same idea as rename_movie: only `name` actually changes."""
        body = {
            "name": theater["name"],
            "city": theater.get("city"),
            "seat_capacity": theater["seat_capacity"],
            "seats_per_row": theater["seats_per_row"],
        }
        return self._request("PUT", "/api/v1/admin/theaters/{}".format(theater["id"]), body)

    # -- scheduling --------------------------------------------------------
    def apply_default_schedule(self, movie_id, theater_id):
        """Append `movie_id` to `theater_id` using the automatic 09:00..21:00
        timeline. No start_time is supplied — the server picks every slot.
        """
        path = "/api/v1/admin/movies/{}/theaters/{}/default-schedule".format(
            movie_id, theater_id)
        return self._request("POST", path)


# ---------------------------------------------------------------------------
# GUI
# ---------------------------------------------------------------------------

class AdminConsole(tk.Tk):
    """The whole application. One window, one notebook, three tabs."""

    def __init__(self):
        super().__init__()
        self.title("MovieBackend Admin Console (unfinished)")
        self.geometry("720x520")
        self.minsize(600, 420)

        self.client = ApiClient(DEFAULT_BASE_URL)
        self.movies = []    # last list_movies() result, cached for lookups
        self.theaters = []  # last list_theaters() result, cached for lookups

        self._build_connection_bar()
        self._build_notebook()
        self._build_status_bar()

        self.after(100, self.refresh_all)

    # -- top bar: server address --------------------------------------------
    def _build_connection_bar(self):
        bar = ttk.Frame(self, padding=(10, 10, 10, 4))
        bar.pack(fill="x")

        ttk.Label(bar, text="Server:").pack(side="left")

        self.base_url_var = tk.StringVar(value=DEFAULT_BASE_URL)
        entry = ttk.Entry(bar, textvariable=self.base_url_var, width=32)
        entry.pack(side="left", padx=(6, 6))

        ttk.Button(bar, text="Refresh", command=self.refresh_all).pack(side="left")

        # TODO: surface AdminService's catalog save/reload here too
        # (POST /api/v1/admin/catalog/save, /catalog/reload) — cheap to add,
        # just not part of the three operations this tool was asked for.

    # -- tabs -----------------------------------------------------------
    def _build_notebook(self):
        self.notebook = ttk.Notebook(self)
        self.notebook.pack(fill="both", expand=True, padx=10, pady=(4, 4))

        self._build_movies_tab()
        self._build_theaters_tab()
        self._build_scheduling_tab()

    def _build_movies_tab(self):
        tab = ttk.Frame(self.notebook, padding=10)
        self.notebook.add(tab, text="Movies")

        tab.columnconfigure(0, weight=1)
        tab.rowconfigure(0, weight=1)

        self.movie_list = tk.Listbox(tab, exportselection=False)
        self.movie_list.grid(row=0, column=0, sticky="nsew")
        self.movie_list.bind("<<ListboxSelect>>", self._on_movie_selected)

        scrollbar = ttk.Scrollbar(tab, orient="vertical", command=self.movie_list.yview)
        scrollbar.grid(row=0, column=1, sticky="ns")
        self.movie_list.configure(yscrollcommand=scrollbar.set)

        form = ttk.Frame(tab)
        form.grid(row=1, column=0, columnspan=2, sticky="ew", pady=(10, 0))
        form.columnconfigure(1, weight=1)

        ttk.Label(form, text="Title:").grid(row=0, column=0, sticky="w")
        self.movie_title_var = tk.StringVar()
        ttk.Entry(form, textvariable=self.movie_title_var).grid(
            row=0, column=1, sticky="ew", padx=(6, 6))
        ttk.Button(form, text="Save name", command=self._rename_selected_movie).grid(
            row=0, column=2)

        # TODO: duration_minutes / language / genre are part of MovieDto too
        # (see Dtos.hpp) but this tool only asked for the name, so they are
        # sent back unchanged rather than exposed as editable fields.
        # TODO: no "add movie" / "delete movie" here — only rename.

    def _build_theaters_tab(self):
        tab = ttk.Frame(self.notebook, padding=10)
        self.notebook.add(tab, text="Theaters")

        tab.columnconfigure(0, weight=1)
        tab.rowconfigure(0, weight=1)

        self.theater_list = tk.Listbox(tab, exportselection=False)
        self.theater_list.grid(row=0, column=0, sticky="nsew")
        self.theater_list.bind("<<ListboxSelect>>", self._on_theater_selected)

        scrollbar = ttk.Scrollbar(tab, orient="vertical", command=self.theater_list.yview)
        scrollbar.grid(row=0, column=1, sticky="ns")
        self.theater_list.configure(yscrollcommand=scrollbar.set)

        form = ttk.Frame(tab)
        form.grid(row=1, column=0, columnspan=2, sticky="ew", pady=(10, 0))
        form.columnconfigure(1, weight=1)

        ttk.Label(form, text="Name:").grid(row=0, column=0, sticky="w")
        self.theater_name_var = tk.StringVar()
        ttk.Entry(form, textvariable=self.theater_name_var).grid(
            row=0, column=1, sticky="ew", padx=(6, 6))
        ttk.Button(form, text="Save name", command=self._rename_selected_theater).grid(
            row=0, column=2)

        # TODO: city / seat_capacity / seats_per_row are editable via the API
        # too (PUT /api/v1/admin/theaters/{id}) but, same as movies, this
        # tool only asked for the name.
        # TODO: no "add theater" / "delete theater" here — only rename.

    def _build_scheduling_tab(self):
        tab = ttk.Frame(self.notebook, padding=10)
        self.notebook.add(tab, text="Scheduling")

        tab.columnconfigure(0, weight=1)
        tab.columnconfigure(1, weight=1)
        tab.rowconfigure(1, weight=1)

        ttk.Label(tab, text="Movie").grid(row=0, column=0, sticky="w")
        ttk.Label(tab, text="Theater").grid(row=0, column=1, sticky="w")

        self.schedule_movie_list = tk.Listbox(tab, exportselection=False)
        self.schedule_movie_list.grid(row=1, column=0, sticky="nsew", padx=(0, 5))

        self.schedule_theater_list = tk.Listbox(tab, exportselection=False)
        self.schedule_theater_list.grid(row=1, column=1, sticky="nsew", padx=(5, 0))

        ttk.Button(
            tab, text="Append to theater (default schedule)",
            command=self._append_movie_to_theater
        ).grid(row=2, column=0, columnspan=2, pady=(10, 4), sticky="ew")

        ttk.Label(
            tab,
            text="Uses the automatic 09:00\u201321:00 timeline; there is no "
                 "way to pick a specific hour here.",
            foreground="#555555",
            wraplength=560,
            justify="left",
        ).grid(row=3, column=0, columnspan=2, sticky="w")

        # TODO: hour/time modification is explicitly out of scope for this
        # tool. A finished version would still need, at minimum:
        #   * POST /api/v1/admin/movies/{m}/theaters/{t}/showtimes
        #     with a chosen "start_time" — appends ONE slot at a picked hour
        #   * PUT  /api/v1/admin/showtimes/{id} with a new "start_time"
        #     — moves an existing slot
        #   * DELETE /api/v1/admin/showtimes/{id} — removes one slot
        #   * GET  /api/v1/admin/movies/{m}/theaters/{t}/showtimes
        #     — to show the admin what is already scheduled before they
        #     pick a new time, so they can avoid an obvious clash
        # None of that is wired up here; "append" only ever calls
        # default-schedule, which never asks for a time.

    # -- status bar -----------------------------------------------------
    def _build_status_bar(self):
        self.status_var = tk.StringVar(value="not connected")
        bar = ttk.Frame(self, padding=(10, 4, 10, 8))
        bar.pack(fill="x")
        ttk.Label(bar, textvariable=self.status_var, foreground="#555555").pack(side="left")

    def _set_status(self, text, is_error=False):
        self.status_var.set(text)
        # TODO: a color-only status line is a poor a11y signal; a finished
        # tool should use ttk.Style states or an icon instead of foreground.

    # -- data loading -----------------------------------------------------
    def refresh_all(self):
        self._refresh_client()
        self._load_movies()
        self._load_theaters()

    def _refresh_client(self):
        self.client = ApiClient(self.base_url_var.get())

    def _load_movies(self):
        try:
            self.movies = self.client.list_movies() or []
        except ApiError as error:
            self._set_status("could not load movies: {}".format(error), is_error=True)
            self.movies = []

        self.movie_list.delete(0, tk.END)
        self.schedule_movie_list.delete(0, tk.END)
        for movie in self.movies:
            label = "{}  \u2014  {}".format(movie["id"], movie["title"])
            self.movie_list.insert(tk.END, label)
            self.schedule_movie_list.insert(tk.END, label)

        self._set_status("loaded {} movie(s)".format(len(self.movies)))

    def _load_theaters(self):
        try:
            self.theaters = self.client.list_theaters() or []
        except ApiError as error:
            self._set_status("could not load theaters: {}".format(error), is_error=True)
            self.theaters = []

        self.theater_list.delete(0, tk.END)
        self.schedule_theater_list.delete(0, tk.END)
        for theater in self.theaters:
            label = "{}  \u2014  {}".format(theater["id"], theater["name"])
            self.theater_list.insert(tk.END, label)
            self.schedule_theater_list.insert(tk.END, label)

        self._set_status("loaded {} theater(s)".format(len(self.theaters)))

    # -- selection handlers -----------------------------------------------
    def _on_movie_selected(self, _event):
        movie = self._selected(self.movie_list, self.movies)
        if movie is not None:
            self.movie_title_var.set(movie["title"])

    def _on_theater_selected(self, _event):
        theater = self._selected(self.theater_list, self.theaters)
        if theater is not None:
            self.theater_name_var.set(theater["name"])

    @staticmethod
    def _selected(listbox, items):
        selection = listbox.curselection()
        if not selection:
            return None
        return items[selection[0]]

    # -- actions -----------------------------------------------------
    def _rename_selected_movie(self):
        movie = self._selected(self.movie_list, self.movies)
        if movie is None:
            messagebox.showinfo("No selection", "Pick a movie from the list first.")
            return

        new_title = self.movie_title_var.get().strip()
        if not new_title:
            messagebox.showwarning("Empty title", "The title cannot be empty.")
            return

        updated = dict(movie)
        updated["title"] = new_title
        try:
            self.client.rename_movie(updated)
        except ApiError as error:
            self._set_status("rename failed: {}".format(error), is_error=True)
            messagebox.showerror("Rename failed", str(error))
            return

        self._set_status("renamed movie {} to '{}'".format(movie["id"], new_title))
        self._load_movies()

    def _rename_selected_theater(self):
        theater = self._selected(self.theater_list, self.theaters)
        if theater is None:
            messagebox.showinfo("No selection", "Pick a theater from the list first.")
            return

        new_name = self.theater_name_var.get().strip()
        if not new_name:
            messagebox.showwarning("Empty name", "The name cannot be empty.")
            return

        updated = dict(theater)
        updated["name"] = new_name
        try:
            self.client.rename_theater(updated)
        except ApiError as error:
            self._set_status("rename failed: {}".format(error), is_error=True)
            messagebox.showerror("Rename failed", str(error))
            return

        self._set_status("renamed theater {} to '{}'".format(theater["id"], new_name))
        self._load_theaters()

    def _append_movie_to_theater(self):
        movie = self._selected(self.schedule_movie_list, self.movies)
        theater = self._selected(self.schedule_theater_list, self.theaters)
        if movie is None or theater is None:
            messagebox.showinfo(
                "No selection", "Pick both a movie and a theater first.")
            return

        if not messagebox.askyesno(
            "Confirm",
            "This replaces any existing screenings of '{}' at '{}' with the "
            "automatic 09:00\u201321:00 timeline. Continue?".format(
                movie["title"], theater["name"])
        ):
            return

        try:
            created = self.client.apply_default_schedule(movie["id"], theater["id"])
        except ApiError as error:
            self._set_status("scheduling failed: {}".format(error), is_error=True)
            messagebox.showerror("Scheduling failed", str(error))
            return

        times = ", ".join(entry["start_time"] for entry in (created or []))
        self._set_status(
            "'{}' appended to '{}' \u2014 {} slot(s): {}".format(
                movie["title"], theater["name"], len(created or []), times))


# TODO: no automated tests accompany this file yet. A finished version
# should get at least a couple of ApiClient tests against a fake urlopen,
# mirroring how tests/smoke/run_smoke_tests.py drives the real server for
# the rest of the project.

# TODO: the admin API in AdminController.hpp has no authentication at all
# (documented gap, see its file header) and this tool does not add any on
# the client side either — anyone who can run this script can reach every
# admin route the client happens to call.


if __name__ == "__main__":
    AdminConsole().mainloop()