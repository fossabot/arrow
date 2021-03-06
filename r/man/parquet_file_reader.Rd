% Generated by roxygen2: do not edit by hand
% Please edit documentation in R/parquet.R
\name{parquet_file_reader}
\alias{parquet_file_reader}
\title{Create a FileReader instance}
\usage{
parquet_file_reader(file, props = parquet_arrow_reader_properties(), ...)
}
\arguments{
\item{file}{file}

\item{props}{reader file properties, as created by \code{\link[=parquet_arrow_reader_properties]{parquet_arrow_reader_properties()}}}

\item{...}{additional parameters}
}
\description{
Create a FileReader instance
}
