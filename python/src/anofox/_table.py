"""
Shared table lifecycle helpers for wrappers that accept table names or DataFrames.
"""

from __future__ import annotations

import functools
import inspect
from typing import Any, Callable, TypeVar, Union

from ._sql import quote_ident
from ._utils import (
    POSITION_COLUMN,
    detect_df_type,
    drop_df_table,
    register_df_as_table,
    registered_view,
)


F = TypeVar("F", bound=Callable[..., Any])

#: A table name, or a pandas/polars DataFrame standing in for one.
TableLike = Union[str, Any]


def resolve_table(conn: Any, table_or_df: Union[str, Any]) -> tuple[str, bool]:
    """
    Return ``(table_name, was_registered)``.

    If *table_or_df* is a DataFrame, it is materialized as a temporary table.
    """
    if isinstance(table_or_df, str):
        return table_or_df, False
    try:
        detect_df_type(table_or_df)  # validate type before registering
    except TypeError:
        raise TypeError(
            f"pass a table name or a DataFrame (pandas or polars), got {_describe(table_or_df)}"
        ) from None
    table_name = register_df_as_table(conn.native, table_or_df)
    return table_name, True


def accepts_dataframes(*param_names: str) -> Callable[[F], F]:
    """
    Decorator: let the parameters *param_names* (table names) also take a
    pandas or polars DataFrame.

    Each DataFrame is copied into a temporary table (see :func:`resolve_table`)
    for the duration of the call and dropped afterwards.
    """
    def decorator(func: F) -> F:
        signature = inspect.signature(func)

        @functools.wraps(func)
        def wrapper(conn: Any, *args: Any, **kwargs: Any) -> Any:
            bound = signature.bind(conn, *args, **kwargs)
            registered: list[str] = []
            try:
                for name in param_names:
                    if name in bound.arguments:
                        table_name, was_registered = resolve_table(conn, bound.arguments[name])
                        bound.arguments[name] = table_name
                        if was_registered:
                            registered.append(table_name)
                return func(*bound.args, **bound.kwargs)
            finally:
                for table_name in registered:
                    cleanup_table(conn, table_name, True)

        return wrapper  # type: ignore[return-value]

    return decorator


def _describe(value: Any) -> str:
    """Short description of *value*'s type for error messages."""
    if value is None:
        return "None"
    cls = type(value)
    module = cls.__module__.split(".")[0]
    return f"{module}.{cls.__qualname__}" if module != "builtins" else cls.__qualname__


def output_df_type(*inputs: Any) -> str:
    """
    DataFrame library for a result: ``"polars"`` when the first DataFrame among
    *inputs* is a polars DataFrame, else ``"pandas"`` (also for table names).
    """
    for value in inputs:
        if not isinstance(value, str):
            return detect_df_type(value)
    return "pandas"


def cleanup_table(conn: Any, table_name: str, was_registered: bool) -> None:
    """Drop a table only when it was created by :func:`resolve_table`."""
    if was_registered:
        drop_df_table(conn.native, table_name)


def apply_scalar(
    conn: Any,
    df: Any,
    df_type: str,
    expression: str,
    params: list,
    result_name: str,
) -> Any:
    """
    Return a copy of *df* with one added column *result_name*.

    *expression* is a SQL expression over the columns of *df* (identifiers
    already quoted) with ``?`` placeholders for *params*. The result keeps the
    input's rows in their order, its other columns unchanged and, for pandas,
    its index, so the new column can be assigned back to the input.
    """
    with registered_view(conn.native, df) as view:
        result = conn.native.execute(
            f"SELECT {expression} AS {quote_ident(result_name)} "
            f"FROM {quote_ident(view)} ORDER BY {quote_ident(POSITION_COLUMN)}",
            params,
        )
        if df_type == "pandas":
            values = result.df()[result_name]
            values.index = df.index
            out = df.copy()
            out[result_name] = values
            return out
        values = result.pl().get_column(result_name)
        return df.with_columns(values)
