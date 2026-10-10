from __future__ import annotations

import re
import unicodedata
from datetime import date as CalendarDate
from datetime import time
from enum import StrEnum
from pathlib import PurePosixPath
from typing import Annotated, Any

from pydantic import (
    BaseModel,
    ConfigDict,
    Field,
    StringConstraints,
    field_validator,
    model_validator,
)
from pydantic.alias_generators import to_camel

from .storage import canonical_json_bytes, normalize_release_path

Identifier = Annotated[
    str,
    StringConstraints(pattern=r"^[a-zA-Z0-9][a-zA-Z0-9._-]{0,127}$"),
]
Title = Annotated[str, StringConstraints(min_length=1, max_length=200)]
PlainText = Annotated[str, StringConstraints(max_length=2000)]

_TIME_PATTERN = re.compile(r"^(?:[01]\d|2[0-3]):[0-5]\d$")


def _normal_text(value: str) -> str:
    normalized = unicodedata.normalize("NFC", value).replace("\r\n", "\n")
    if "\x00" in normalized:
        raise ValueError("Text must not contain NUL")
    return normalized


class ContentModel(BaseModel):
    model_config = ConfigDict(
        alias_generator=to_camel,
        populate_by_name=True,
        extra="forbid",
        str_strip_whitespace=True,
    )


class LayoutMode(StrEnum):
    DASHBOARD = "dashboard"
    FULLSCREEN_MEDIA = "fullscreen_media"


class MediaFit(StrEnum):
    FIT = "fit"
    CROP = "crop"


class MediaKind(StrEnum):
    VIDEO = "video"


class SafeInsets(ContentModel):
    """Fractional insets applied to the physical display's logical geometry."""

    left: float = Field(default=0, ge=0, le=0.1)
    top: float = Field(default=0, ge=0, le=0.1)
    right: float = Field(default=0, ge=0, le=0.1)
    bottom: float = Field(default=0, ge=0, le=0.1)


class AudioPolicy(ContentModel):
    muted: bool = False
    volume: float = Field(default=1, ge=0, le=1)


class DisplayConfig(ContentModel):
    schema_version: int = Field(default=1, ge=1, le=1)
    layout_mode: LayoutMode = LayoutMode.DASHBOARD
    default_media_fit: MediaFit = MediaFit.FIT
    safe_insets: SafeInsets = Field(default_factory=SafeInsets)
    audio: AudioPolicy = Field(default_factory=AudioPolicy)


class PlaylistItem(ContentModel):
    id: Identifier
    order: int = Field(ge=0, le=10000)
    kind: MediaKind = MediaKind.VIDEO
    path: str
    fit: MediaFit | None = None
    muted: bool | None = None
    volume: float | None = Field(default=None, ge=0, le=1)

    @field_validator("path")
    @classmethod
    def validate_path(cls, value: str) -> str:
        normalized = unicodedata.normalize("NFC", value)
        safe = normalize_release_path(normalized)
        if not safe.startswith("media/"):
            raise ValueError("Playlist media must be under media/")
        return safe


class PlaylistConfig(ContentModel):
    schema_version: int = Field(default=1, ge=1, le=1)
    repeat: str = Field(default="all", pattern=r"^all$")
    items: list[PlaylistItem] = Field(default_factory=list, max_length=1000)

    @model_validator(mode="after")
    def validate_items(self) -> PlaylistConfig:
        ids = [item.id for item in self.items]
        orders = [item.order for item in self.items]
        paths = [item.path.casefold() for item in self.items]
        if len(ids) != len(set(ids)):
            raise ValueError("Playlist item IDs must be unique")
        if len(orders) != len(set(orders)):
            raise ValueError("Playlist item orders must be unique")
        if len(paths) != len(set(paths)):
            raise ValueError("Playlist paths must be unique ignoring case")
        self.items.sort(key=lambda item: item.order)
        return self


class ScheduleEvent(ContentModel):
    id: Identifier
    title: Title
    location: Annotated[str, StringConstraints(min_length=1, max_length=100)]
    days_of_week: list[int] | None = Field(default=None, min_length=1, max_length=7)
    date: CalendarDate | None = None
    start_time: str
    end_time: str
    ends_next_day: bool = False
    priority: int = Field(default=0, ge=-1000, le=1000)

    @field_validator("title", "location")
    @classmethod
    def normalize_text(cls, value: str) -> str:
        return _normal_text(value)

    @field_validator("start_time", "end_time")
    @classmethod
    def validate_time(cls, value: str) -> str:
        if not _TIME_PATTERN.fullmatch(value):
            raise ValueError("Time must use 24-hour HH:MM")
        return value

    @field_validator("days_of_week")
    @classmethod
    def validate_days(cls, value: list[int] | None) -> list[int] | None:
        if value is None:
            return None
        if any(day < 1 or day > 7 for day in value):
            raise ValueError("daysOfWeek uses ISO values 1 (Monday) through 7 (Sunday)")
        if len(value) != len(set(value)):
            raise ValueError("daysOfWeek must not contain duplicates")
        return sorted(value)

    @model_validator(mode="after")
    def validate_interval(self) -> ScheduleEvent:
        if (self.days_of_week is None) == (self.date is None):
            raise ValueError("Specify exactly one of daysOfWeek or date")
        start = time.fromisoformat(self.start_time)
        end = time.fromisoformat(self.end_time)
        if start == end:
            raise ValueError("Schedule start and end must be different")
        if end < start and not self.ends_next_day:
            raise ValueError("Overnight schedules must set endsNextDay")
        if end > start and self.ends_next_day:
            raise ValueError("endsNextDay is only valid when endTime is earlier than startTime")
        return self


class ScheduleConfig(ContentModel):
    schema_version: int = Field(default=1, ge=1, le=1)
    timezone: str = Field(default="Asia/Seoul", pattern=r"^Asia/Seoul$")
    events: list[ScheduleEvent] = Field(default_factory=list, max_length=5000)

    @model_validator(mode="after")
    def validate_events(self) -> ScheduleConfig:
        ids = [event.id for event in self.events]
        if len(ids) != len(set(ids)):
            raise ValueError("Schedule event IDs must be unique")
        return self


class SubtitleSize(StrEnum):
    SMALL = "small"
    NORMAL = "normal"
    LARGE = "large"


class SubtitleMark(StrEnum):
    BOLD = "bold"
    ITALIC = "italic"
    UNDERLINE = "underline"
    STRIKE = "strike"


class SubtitleEffect(StrEnum):
    FADE = "fade"
    SLIDE_LEFT = "slide_left"
    SLIDE_UP = "slide_up"
    SOFT_ZOOM = "soft_zoom"


_SUBTITLE_MARK_ORDER = {
    SubtitleMark.BOLD: 0,
    SubtitleMark.ITALIC: 1,
    SubtitleMark.UNDERLINE: 2,
    SubtitleMark.STRIKE: 3,
}


class SubtitleSpan(ContentModel):
    text: Annotated[str, StringConstraints(min_length=1, max_length=2000)]
    size: SubtitleSize = SubtitleSize.NORMAL
    marks: list[SubtitleMark] = Field(default_factory=list, max_length=4)

    @field_validator("text")
    @classmethod
    def normalize_text(cls, value: str) -> str:
        normalized = _normal_text(value)
        if "\n" in normalized:
            raise ValueError("Subtitle span text must not contain line breaks")
        return normalized

    @field_validator("marks")
    @classmethod
    def normalize_marks(cls, value: list[SubtitleMark]) -> list[SubtitleMark]:
        if len(value) != len(set(value)):
            raise ValueError("Subtitle marks must not contain duplicates")
        return sorted(value, key=_SUBTITLE_MARK_ORDER.__getitem__)


class SubtitleLine(ContentModel):
    spans: list[SubtitleSpan] = Field(min_length=1, max_length=200)

    @model_validator(mode="after")
    def merge_adjacent_spans(self) -> SubtitleLine:
        merged: list[SubtitleSpan] = []
        for span in self.spans:
            if (
                merged
                and merged[-1].size == span.size
                and merged[-1].marks == span.marks
                and len(merged[-1].text) + len(span.text) <= 2000
            ):
                merged[-1] = merged[-1].model_copy(
                    update={"text": merged[-1].text + span.text}
                )
            else:
                merged.append(span)
        self.spans = merged
        return self


class SubtitleMessage(ContentModel):
    id: Identifier
    duration_ms: int = Field(default=5000, ge=1000, le=60000)
    effect: SubtitleEffect = SubtitleEffect.FADE
    font_size_px: int = Field(default=60, ge=24, le=96)
    transition_ms: int = Field(default=700, ge=150, le=3000)
    text: PlainText | None = None
    lines: list[SubtitleLine] | None = Field(
        default=None,
        min_length=1,
        max_length=2,
    )

    @field_validator("text")
    @classmethod
    def normalize_text(cls, value: str | None) -> str | None:
        return _normal_text(value) if value is not None else None

    @model_validator(mode="after")
    def validate_body(self) -> SubtitleMessage:
        if (self.text is None) == (self.lines is None):
            raise ValueError("Subtitle message must contain exactly one of text or lines")
        if self.lines is not None:
            total = sum(len(span.text) for line in self.lines for span in line.spans)
            if total > 80:
                raise ValueError("Formatted subtitle text must be 80 characters or less")
        return self


class SubtitleConfig(ContentModel):
    schema_version: int = Field(default=1, ge=1, le=2)
    messages: list[SubtitleMessage] = Field(default_factory=list, max_length=1000)

    @model_validator(mode="after")
    def validate_messages(self) -> SubtitleConfig:
        ids = [message.id for message in self.messages]
        if len(ids) != len(set(ids)):
            raise ValueError("Subtitle message IDs must be unique")
        if self.schema_version == 1 and any(
            message.text is None for message in self.messages
        ):
            raise ValueError("Subtitle schema v1 messages must use text")
        if self.schema_version == 2 and any(
            message.lines is None for message in self.messages
        ):
            raise ValueError("Subtitle schema v2 messages must use lines")
        return self


class ActivationPolicy(ContentModel):
    settings: str = Field(default="immediate", pattern=r"^immediate$")
    playlist: str = Field(default="after_current_item", pattern=r"^after_current_item$")
    emergency: str = Field(default="immediate", pattern=r"^immediate$")


class ContentSpec(ContentModel):
    display: DisplayConfig = Field(default_factory=DisplayConfig)
    playlist: PlaylistConfig = Field(default_factory=PlaylistConfig)
    schedule: ScheduleConfig = Field(default_factory=ScheduleConfig)
    subtitle: SubtitleConfig = Field(default_factory=SubtitleConfig)
    activation: ActivationPolicy = Field(default_factory=ActivationPolicy)
    allow_empty_playlist: bool = False

    @model_validator(mode="after")
    def validate_content(self) -> ContentSpec:
        if not self.playlist.items and not self.allow_empty_playlist:
            raise ValueError("Playlist must contain at least one item")
        return self

    def referenced_media_paths(self) -> set[str]:
        return {item.path for item in self.playlist.items}


def compile_content_bundle(
    spec: ContentSpec,
    available_media_paths: set[str],
) -> dict[str, bytes]:
    normalized_available = {
        normalize_release_path(unicodedata.normalize("NFC", path)) for path in available_media_paths
    }
    missing = sorted(spec.referenced_media_paths() - normalized_available)
    if missing:
        raise ValueError(f"Playlist references missing media: {', '.join(missing)}")

    bundle: dict[str, Any] = {
        "schemaVersion": 1,
        "display": "display.json",
        "playlist": "playlist.json",
        "schedule": "schedule.json",
        "subtitle": "subtitle.json",
        "activation": spec.activation.model_dump(by_alias=True),
    }
    return {
        "bundle.json": canonical_json_bytes(bundle),
        "display.json": canonical_json_bytes(spec.display.model_dump(by_alias=True)),
        "playlist.json": canonical_json_bytes(spec.playlist.model_dump(by_alias=True)),
        "schedule.json": canonical_json_bytes(spec.schedule.model_dump(by_alias=True, mode="json")),
        "subtitle.json": canonical_json_bytes(
            spec.subtitle.model_dump(by_alias=True, exclude_none=True)
        ),
    }


def portable_media_name(original_name: str, sha256: str, suffix: str) -> str:
    stem = PurePosixPath(unicodedata.normalize("NFC", original_name)).stem
    safe_stem = re.sub(r"[^0-9A-Za-z가-힣._-]+", "-", stem).strip(".-") or "media"
    normalized_suffix = suffix.lower().lstrip(".")
    if not re.fullmatch(r"[a-z0-9]{2,8}", normalized_suffix):
        raise ValueError("Invalid media suffix")
    return f"media/{safe_stem[:80]}-{sha256[:12]}.{normalized_suffix}"
