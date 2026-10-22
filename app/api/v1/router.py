from pathlib import Path

from fastapi import APIRouter, Request, Query
from fastapi.concurrency import run_in_threadpool
from fastapi.responses import HTMLResponse, PlainTextResponse, Response
from fastapi.templating import Jinja2Templates

from app.core.config import settings
from app.services import clock, weather as weather_svc, jewish_cal as jewish_cal_svc
from app.services import seo as seo_svc

router = APIRouter()

DEFAULT_FONT = clock.DEFAULT_FONT

# Path(__file__) = app/api/v1/router.py  →  .parent×3 = app/
_TEMPLATES = Jinja2Templates(
    directory=str(Path(__file__).parent.parent.parent / "templates")
)


@router.get("/", response_class=HTMLResponse, include_in_schema=False)
async def home(
    request: Request,
    font: str = Query(default=clock.DEFAULT_FONT),
    location: str = Query(default="Tel Aviv"),
    calendar: str = Query(default="gregorian"),
) -> HTMLResponse:
    base_url = str(request.base_url).rstrip("/")
    return _TEMPLATES.TemplateResponse(
        request,
        "index.html",
        {
            "fonts": sorted(clock.VALID_FONTS),
            "default_font": font,
            "default_location": location,
            "default_calendar": calendar,
            "gtag_id": settings.gtag_id,
            "base_url": base_url,
        },
    )


@router.get("/robots.txt", response_class=PlainTextResponse, include_in_schema=False)
async def robots_txt(request: Request) -> PlainTextResponse:
    base_url = str(request.base_url).rstrip("/")
    return PlainTextResponse(seo_svc.generate_robots(base_url))


@router.get("/sitemap.xml", include_in_schema=False)
async def sitemap_xml(request: Request) -> Response:
    base_url = str(request.base_url).rstrip("/")
    return Response(
        content=seo_svc.generate_sitemap(base_url),
        media_type="application/xml",
    )


@router.get(
    "/clock",
    responses={200: {"content": {"image/png": {}}}},
    response_class=Response,
)
@router.get(
    "/clock.png",
    responses={200: {"content": {"image/png": {}}}},
    response_class=Response,
)
async def get_clock(
    request:  Request,
    font:      str = Query(default=DEFAULT_FONT),
    sleeptime: str = Query(default="0"),
    location:  str = Query(default="Tel Aviv"),
    calendar:  str = Query(default="gregorian"),
) -> Response:
    loc = location or "Tel Aviv"
    w = await weather_svc.get_weather(loc, request.app.state.http_client)

    # The Hebrew date is always shown (header or footer), so always fetch it (cached 24 h).
    today = clock.get_israel_time().date()
    jdate = await jewish_cal_svc.get_jewish_date(today, request.app.state.http_client)

    img_bytes = await run_in_threadpool(
        clock.generate_clock_image,
        font_name   = font,
        sleep_time  = sleeptime == "1",
        weather     = w,
        jewish_date = jdate,
        calendar    = calendar,
    )
    return Response(
        content=img_bytes,
        media_type="image/png",
        headers={"Cache-Control": "no-cache"},
    )
